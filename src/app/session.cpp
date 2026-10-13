#include "app/session.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "net/wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

namespace garnet {
namespace {
constexpr const char* kTag = "session";
constexpr const char* kNs = "garnet";
constexpr uint32_t kProbeEveryMs = 1000;
constexpr uint32_t kRetryBaseMs = 2000;  // first retry after a failed handshake or link
constexpr uint32_t kRetryMaxMs = 60000;  // doubling stops here
constexpr uint32_t kWelcomeTimeoutMs = 5000;
constexpr uint32_t kHandshakeTimeoutMs = 15000;  // the readiness gate's own bound (§6.4)
constexpr uint32_t kStaleMs = 3000;  // ~3 idle PING intervals with no answer
constexpr uint32_t kNudgeHoldMs = 1000;
constexpr uint32_t kLossShowMs = 10000;

void nvsBlob(const char* key, void* out, size_t len) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = len;
    nvs_get_blob(h, key, out, &n);
    nvs_close(h);
}

void nvsPutBlob(const char* key, const void* in, size_t len) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
    if (len == 0) {
        nvs_erase_key(h, key);
    } else {
        nvs_set_blob(h, key, in, len);
    }
    nvs_commit(h);
    nvs_close(h);
}

uint32_t timeDiff(uint32_t now, uint32_t then) { return now - then; }
bool due(uint32_t now, uint32_t at) { return int32_t(now - at) >= 0; }

template <typename T>
T* psramNew() {
    void* p = heap_caps_malloc(sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    configASSERT(p);
    return new (p) T();
}
}  // namespace

uint32_t Session::EspClock::nowUs() const { return uint32_t(esp_timer_get_time()); }
uint32_t Session::EspRandom::nextU32() { return esp_random(); }
void Session::EspRandom::fill(std::span<std::byte> out) { esp_fill_random(out.data(), out.size()); }

bool Session::sendDatagram(void* ctx, std::span<const std::byte> d) {
    auto* self = static_cast<Session*>(ctx);
    const bool bcast = self->_udp.sendTo(wifi::broadcast_ipv4(), d);
    const bool uni = self->_udp.sendTo(self->_activeIp, d);
    return bcast || uni;
}

void Session::begin() {
    nvsBlob("iid", _id.instance_id.data(), _id.instance_id.size());
    bool zero = true;
    for (auto b : _id.instance_id) zero = zero && b == std::byte{0};
    if (zero) {
        esp_fill_random(_id.instance_id.data(), _id.instance_id.size());
        nvsPutBlob("iid", _id.instance_id.data(), _id.instance_id.size());
    }
    nvsBlob("token", _id.token.data(), _id.token.size());
    for (auto b : _id.token) _id.hasToken = _id.hasToken || b != std::byte{0};
    nvsBlob("hub_iid", &_savedHubId, sizeof(_savedHubId));
    nvsBlob("hub_ip", &_hubIp, sizeof(_hubIp));
    nvsBlob("hub_port", &_hubPort, sizeof(_hubPort));
    if (_hubIp != 0) std::snprintf(_hubName, sizeof(_hubName), "typed address");
    uint8_t m = 0;
    nvsBlob("link", &m, 1);
    _mode = m == 1 ? LinkMode::Tether : LinkMode::Wifi;

    uint8_t mac[6]{};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    std::snprintf(_name, sizeof(_name), "Garnet %02X%02X", mac[4], mac[5]);
    _id.client_kind = "remote";
    _id.client_name = _name;

    _model = psramNew<Model>();
    _datagram = valence::EstopDatagramSender(&Session::sendDatagram, this, uint16_t(esp_random()));
    buildClient();
    ESP_LOGI(kTag, "%s, %s, link %s", _name, _id.hasToken ? "paired" : "unpaired (watch)",
             _mode == LinkMode::Tether ? "tether" : "wifi");
}

void Session::buildClient() {
    std::array<std::byte, valence::limits::etag_bytes> etag{};
    bool haveEtag = false;
    if (_client) {
        if (_model->loaded()) {
            const auto e = _client->readyEtag();
            std::copy(e.begin(), e.end(), etag.begin());
            haveEtag = true;
        }
        _client->disconnect();
        _client->~Client();
        heap_caps_free(_client);
        _client = nullptr;
    }
    void* p = heap_caps_malloc(sizeof(valence::Client), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    configASSERT(p);
    valence::ITransport& t = _mode == LinkMode::Tether ? static_cast<valence::ITransport&>(_tether)
                                                        : static_cast<valence::ITransport&>(_ws);
    _client = new (p) valence::Client(_id, t, _clock, _rng, *this);
    _client->addSubscriptionWish(valence::channels::safety, 0.0f, valence::Priority::critical);
    _client->setClientVersion("garnet-0.1.0");
    if (haveEtag) _client->setCachedEtag(std::span<const std::byte, valence::limits::etag_bytes>(etag));
    _retryAtMs = _nowMs;
}

void Session::setMode(LinkMode m) {
    if (m == _mode) return;
    _mode = m;
    const uint8_t v = uint8_t(m);
    nvsPutBlob("link", &v, 1);
    if (m == LinkMode::Tether) _ws.stop();
    _rebuild = true;
}

void Session::connectTo(uint32_t ipv4, uint16_t port) {
    _hubIp = ipv4;
    _hubPort = port;
    nvsPutBlob("hub_ip", &_hubIp, sizeof(_hubIp));
    nvsPutBlob("hub_port", &_hubPort, sizeof(_hubPort));
    _client->disconnect();
    _backoffMs = 0;
    _retryAtMs = _nowMs;
    std::snprintf(_hubName, sizeof(_hubName), "typed address");
    if (_mode == LinkMode::Wifi && wifi::state() == wifi::State::Connected) {
        _activeIp = ipv4;
        _ws.start(ipv4, port);
    }
}

void Session::forgetHub() {
    _savedHubId = 0;
    nvsPutBlob("hub_iid", nullptr, 0);
    nvsPutBlob("hub_ip", nullptr, 0);
    nvsPutBlob("hub_port", nullptr, 0);
    _client->disconnect();
    _ws.stop();
    _hubIp = 0;
    _hubName[0] = '\0';
}

void Session::setStatus(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(_status, sizeof(_status), fmt, ap);
    va_end(ap);
}

void Session::tickWifi(uint32_t nowMs) {
    const auto ws = wifi::state();
    if (ws != wifi::State::Connected) {
        if (_client->state() != valence::ClientSessionState::CLOSED) _client->disconnect();
        _udp.close();
        if (ws == wifi::State::Portal) {
            setStatus("WiFi setup: join %s", wifi::portal_ssid());
        } else {
            setStatus("WiFi: joining");
        }
        return;
    }
    if (!_udp.isOpen()) _udp.open();
    if (!due(nowMs, _retryAtMs)) {  // backing off: no socket, no probe
        setStatus("Retrying %s in %u s", _hubName[0] ? _hubName : "hub", unsigned((_retryAtMs - nowMs + 999) / 1000));
        return;
    }
    if (!_ws.started() && _hubIp != 0) {  // a typed address outranks discovery
        _activeIp = _hubIp;
        _ws.start(_hubIp, _hubPort);
        return;
    }
    if (!_ws.started()) {
        uint32_t ip = 0;
        uint16_t port = 0;
        uint64_t iid = 0;
        char name[33];
        while (_udp.pollReply(ip, port, name, sizeof(name), iid)) {
            if (_savedHubId != 0 && iid != _savedHubId) continue;
            ESP_LOGI(kTag, "hub \"%s\" at %u.%u.%u.%u:%u", name, unsigned(ip & 0xFF), unsigned((ip >> 8) & 0xFF),
                     unsigned((ip >> 16) & 0xFF), unsigned(ip >> 24), unsigned(port));
            std::memcpy(_hubName, name, sizeof(_hubName));
            if (_savedHubId == 0 && iid != 0) {
                _savedHubId = iid;
                nvsPutBlob("hub_iid", &iid, sizeof(iid));
            }
            _activeIp = ip;
            _ws.start(ip, port);
            return;
        }
        if (timeDiff(nowMs, _lastProbeMs) >= kProbeEveryMs) {
            _lastProbeMs = nowMs;
            _udp.sendProbe(wifi::broadcast_ipv4());
            setStatus("Looking for a hub");
        }
        return;
    }
    const auto st = _client->state();
    if (_ws.connected() && st == valence::ClientSessionState::CLOSED && due(nowMs, _retryAtMs)) {
        _connectStartMs = nowMs;
        _client->connect();
    } else if (!_ws.connected() && st != valence::ClientSessionState::CLOSED) {
        scheduleRetry(0, "link down");
    }
    if (!_ws.connected()) setStatus("Connecting to %s", _hubName[0] ? _hubName : "hub");
}

void Session::tick(uint32_t nowUs) {
    _nowMs = uint32_t(esp_timer_get_time() / 1000);
    if (_rebuild) {
        _rebuild = false;
        buildClient();
    }
    if (_mode == LinkMode::Wifi) {
        tickWifi(_nowMs);
    } else {
        if (_client->state() == valence::ClientSessionState::CLOSED && due(_nowMs, _retryAtMs)) {
            _connectStartMs = _nowMs;
            _client->connect();
            setStatus("Tether: waiting for the bridge");
        }
    }
    _client->update(nowUs);
    if (_busyRetry) {
        _busyRetry = false;
        scheduleRetry(_busyRetryMs, "hub busy");
    }
    if (_stallOnSyncMs != 0 && _client->state() == valence::ClientSessionState::SYNCING) {
        ESP_LOGW(kTag, "bench: UI task stalls %u ms inside the catalog transfer", unsigned(_stallOnSyncMs));
        vTaskDelay(pdMS_TO_TICKS(_stallOnSyncMs));
        _stallOnSyncMs = 0;
    }

    const auto st = _client->state();
    const uint32_t handshakeMs = timeDiff(_nowMs, _connectStartMs);
    if ((st == valence::ClientSessionState::HELLO_SENT && handshakeMs > kWelcomeTimeoutMs) ||
        (st == valence::ClientSessionState::SYNCING && handshakeMs > kHandshakeTimeoutMs)) {
        scheduleRetry(0, "handshake timed out");
    }
    if (st == valence::ClientSessionState::LIVE && linkStale()) {
        ESP_LOGW(kTag, "no frames for %u ms", unsigned(_client->rxAgeMs()));
        scheduleRetry(0, "link stale");
    }
    if (_needSubscribe && st == valence::ClientSessionState::LIVE) {
        _needSubscribe = false;
        const auto* pos = _model->position();
        for (uint16_t ch : _model->watched()) {
            _client->subscribe(ch, pos && pos->ch == ch ? 10.0f : 0.0f, valence::Priority::normal);
        }
    }
    if (st == valence::ClientSessionState::LIVE) setStatus("Live: %s", _hubName[0] ? _hubName : "hub");

    _datagram.update(_nowMs);
    if (_haltPending && estopLatched()) _haltPending = false;

    const uint32_t lost = _ws.unsent() + _writesLost;
    if (lost != _lossSeen) {
        ESP_LOGW(kTag, "%u frame(s) or write(s) lost to the link (total %u)", unsigned(lost - _lossSeen), unsigned(lost));
        _lossSeen = lost;
        _lossAtMs = _nowMs;
    }
}

void Session::printLinkStats() const {
    std::printf("link: ws %s, unsent %u, rx dropped %u, writes lost %u, catalog requests %u, rx age %u ms\n",
                _ws.connected() ? "up" : "down", unsigned(_ws.unsent()), unsigned(_ws.rxDropped()),
                unsigned(_writesLost), unsigned(_client->catalogReqCount()), unsigned(_client->rxAgeMs()));
}

uint32_t Session::lostRecently() const {
    return _lossAtMs != 0 && timeDiff(_nowMs, _lossAtMs) < kLossShowMs ? _lossSeen : 0;
}

bool Session::linkStale() const { return live() && _client->rxAgeMs() > kStaleMs; }

bool Session::estopLatched() const {
    const auto w = _client->safetyWord();
    return w && (*w & valence::safety_bits::ESTOP);
}

bool Session::haltUnconfirmed() const {
    return _haltPending && (_client->estopSendFailed() || (!_datagram.active() && !live()));
}

void Session::halt() {
    _haltPending = true;
    _haltAtMs = _nowMs;
    _client->halt();  // in a session: raw frame until latched, plus the estop op when LIVE
    if (!live() && _udp.isOpen()) {
        _datagram.initiate(valence::safety_causes::user, uint8_t(_client->roles()), _nowMs);
    }
    ESP_LOGW(kTag, "HALT sent (%s)", live() ? "session" : "datagram");
}

void Session::pauseToggle() {
    if (!live()) return;
    if (pauseLatched()) {
        _client->resume();
    } else {
        _client->pause();
    }
}

void Session::releaseHalt() {
    if (live() && estopLatched()) _client->release();
}

void Session::knock() {
    if (live() && _client->sendPairKnock()) {
        ESP_LOGI(kTag, "knock sent: approve it in a configure client (Phosphor: Pairing, Pending knocks)");
    }
}

void Session::askConfigure() {
    if (live() && _client->sendPairKnock(valence::AccessLevel::configure)) {
        ESP_LOGI(kTag, "configure ask sent: double-press the hub's Pair button");
    }
}

bool Session::write(const Setting& s, float physical) {
    if (!live() || !_model->enabled(s, roles())) {
        ESP_LOGW(kTag, "write %.*s refused locally: %s", int(s.name().size()), s.name().data(),
                 live() ? "disabled for this tier or by its mask" : "no live session");
        return false;
    }
    const auto v = _model->encode(s, physical);
    const auto id = v ? _client->sendIntent(s.intentCh, *v) : std::nullopt;
    ESP_LOGI(kTag, "write 0x%04x.%u %.*s = %g: %s", unsigned(s.intentCh), unsigned(s.sf->key), int(s.name().size()),
             s.name().data(), double(physical), id ? "sent" : "not sent");
    if (!id) return false;
    _lastWrite = &s;
    _lastWriteValue = physical;
    _lastWriteMs = _nowMs;
    return true;
}

float Session::current(const Setting& s) {
    if (_lastWrite == &s && _nowMs - _lastWriteMs < kNudgeHoldMs) return _lastWriteValue;
    return _model->value(s).value_or(s.min());
}

bool Session::nudge(const Setting& s, int dir) {
    float v = current(s) + float(dir) * s.step();
    if (v < s.min()) v = s.min();
    if (v > s.max()) v = s.max();
    return write(s, v);
}

bool Session::toggle(const Setting& s) { return write(s, current(s) >= 0.5f ? 0.0f : 1.0f); }

bool Session::runAction(std::string_view a) {
    if (a == "halt") {
        halt();
        return true;
    }
    if (a == "pause") {
        pauseToggle();
        return true;
    }
    struct Role {
        const char* up;
        const char* down;
        const char* role;
    };
    constexpr Role kRoles[] = {{"speed+", "speed-", "pattern.speed"},
                               {"stroke+", "stroke-", "pattern.stroke"},
                               {"wmin+", "wmin-", "window.min"},
                               {"wmax+", "wmax-", "window.max"}};
    for (const auto& r : kRoles) {
        if (a == r.up || a == r.down) {
            const Setting* s = _model->byRole(r.role);
            if (!s) ESP_LOGW(kTag, "%.*s: this catalog has no setting with role %s", int(a.size()), a.data(), r.role);
            return s && nudge(*s, a == r.up ? 1 : -1);
        }
    }
    // "<op>:<channel hex>.<key>"
    const size_t colon = a.find(':');
    const size_t dot = a.find('.');
    if (colon == std::string_view::npos || dot == std::string_view::npos || dot < colon) return false;
    char buf[16]{};
    const std::string_view chs = a.substr(colon + 1, dot - colon - 1);
    const std::string_view keys = a.substr(dot + 1);
    if (chs.size() >= sizeof(buf) || keys.size() >= sizeof(buf)) return false;
    std::memcpy(buf, chs.data(), chs.size());
    const auto ch = uint16_t(std::strtoul(buf, nullptr, 16));
    std::memset(buf, 0, sizeof(buf));
    std::memcpy(buf, keys.data(), keys.size());
    const auto key = uint8_t(std::strtoul(buf, nullptr, 10));
    const Setting* s = _model->byWrite(ch, key);
    if (!s) return false;
    const std::string_view op = a.substr(0, colon);
    if (op == "inc") return nudge(*s, 1);
    if (op == "dec") return nudge(*s, -1);
    if (op == "toggle") return toggle(*s);
    return false;
}

// ---- ClientDelegate -------------------------------------------------------------

void Session::onStateChange(valence::ClientSessionState s) {
    ESP_LOGI(kTag, "session state %d, roles %d", int(s), int(_client->roles()));
    if (s == valence::ClientSessionState::LIVE) {
        _needSubscribe = _model->loaded();
        _backoffMs = 0;
    }
    if (s == valence::ClientSessionState::CLOSED) _model->clearValues();
}

void Session::onState(uint16_t ch, uint16_t, std::span<const std::byte> payload) { _model->onState(ch, payload); }

void Session::onNack(const valence::NackMsg& n) {
    ESP_LOGW(kTag, "NACK code 0x%04x on channel 0x%04x, retry_after %u ms", unsigned(n.code), unsigned(n.channel_id),
             n.has_retry_after_ms ? unsigned(n.retry_after_ms) : 0u);
    // BUSY before LIVE refuses the catalog fetch: leave and come back later,
    // never re-ask at once (acted on in tick(), outside the Client's dispatch).
    if (n.code == valence::NackCode::BUSY && !live()) {
        _busyRetry = true;
        _busyRetryMs = n.has_retry_after_ms ? n.retry_after_ms : 0;
    }
}

void Session::scheduleRetry(uint32_t atLeastMs, const char* why) {
    _backoffMs = _backoffMs == 0 ? kRetryBaseMs : (_backoffMs * 2 > kRetryMaxMs ? kRetryMaxMs : _backoffMs * 2);
    const uint32_t base = atLeastMs > _backoffMs ? atLeastMs : _backoffMs;
    const uint32_t delay = base + esp_random() % (base / 4 + 1);  // jitter: two remotes never retry in step
    _retryAtMs = _nowMs + delay;
    if (_client->state() != valence::ClientSessionState::CLOSED) _client->disconnect();
    _ws.stop();
    ESP_LOGW(kTag, "%s: retrying in %u ms", why, unsigned(delay));
}

void Session::onPairGrant(std::span<const std::byte> token, valence::AccessLevel r) {
    if (token.size() != _id.token.size()) return;
    std::copy(token.begin(), token.end(), _id.token.begin());
    _id.hasToken = true;
    nvsPutBlob("token", _id.token.data(), _id.token.size());
    ESP_LOGI(kTag, "paired, roles %d: reconnecting with the token", int(r));
    _rebuild = true;
}

void Session::onCatalog(std::span<const std::byte> encoded, bool verified) {
    ESP_LOGI(kTag, "catalog %u B (%s)", unsigned(encoded.size()), verified ? "verified" : "hash mismatch, degraded");
    _model->load(encoded);
}

}  // namespace garnet
