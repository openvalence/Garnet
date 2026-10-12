#include <cerrno>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "link/link.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"

namespace garnet {
namespace {

constexpr const char* kTag = "ws";
constexpr size_t kTxDepth = 64;
constexpr size_t kRxDepth = 192;  // a whole catalog burst (~140 chunks) fits
constexpr uint32_t kReconnectMs = 2000;
constexpr uint32_t kReconnectMaxMs = 30000;
constexpr int kSendTimeoutMs = 3000;
constexpr int kRecvPollMs = 500;  // the reader wakes this often to see a drop request

enum : uint8_t { kOpCont = 0x0, kOpText = 0x1, kOpBin = 0x2, kOpClose = 0x8, kOpPing = 0x9, kOpPong = 0xA };

struct TxItem {
    uint8_t opcode;
    uint16_t len;
    std::byte data[valence::kFrameBufferCapacity];
};

void readerTrampoline(void* self) { static_cast<WsLink*>(self)->readerTask(); }
void writerTrampoline(void* self) { static_cast<WsLink*>(self)->writerTask(); }

void setTimeout(int fd, int opt, int ms) {
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, opt, &tv, sizeof(tv));
}

bool sendAll(int fd, const uint8_t* p, size_t n) {
    while (n > 0) {
        const ssize_t k = ::send(fd, p, n, 0);
        if (k <= 0) return false;
        p += k;
        n -= size_t(k);
    }
    return true;
}

}  // namespace

// ---- FrameQueue ---------------------------------------------------------------

FrameQueue::FrameQueue(size_t depth)
    : _q(xQueueCreateWithCaps(depth, sizeof(valence::FrameBuffer), MALLOC_CAP_SPIRAM)) {
    configASSERT(_q);
}

bool FrameQueue::push(std::span<const std::byte> frame) {
    if (frame.size() > valence::kFrameBufferCapacity) {
        _dropped.fetch_add(1);
        return false;
    }
    const valence::FrameBuffer fb = valence::FrameBuffer::from(frame);
    if (xQueueSend(_q, &fb, 0) != pdPASS) {
        _dropped.fetch_add(1);
        return false;
    }
    return true;
}

std::optional<valence::FrameBuffer> FrameQueue::pop() {
    valence::FrameBuffer fb;
    if (xQueueReceive(_q, &fb, 0) != pdPASS) return std::nullopt;
    return fb;
}

void FrameQueue::clear() { xQueueReset(_q); }

// ---- WsLink: UI-task side -------------------------------------------------------

WsLink::WsLink() : _rx(kRxDepth), _tx(xQueueCreateWithCaps(kTxDepth, sizeof(TxItem), MALLOC_CAP_SPIRAM)) {
    configASSERT(_tx);
}

void WsLink::start(uint32_t ipv4, uint16_t port) {
    _port = port;
    _ip = ipv4;
    drop();
    if (!_tasksStarted) {
        _tasksStarted = true;
        xTaskCreatePinnedToCoreWithCaps(readerTrampoline, "ws_rx", 6144, this, 6, nullptr, 0, MALLOC_CAP_INTERNAL);
        xTaskCreatePinnedToCoreWithCaps(writerTrampoline, "ws_tx", 4096, this, 6, nullptr, 0, MALLOC_CAP_INTERNAL);
    }
}

void WsLink::stop() {
    _ip = 0;
    _closing = true;
    drop();
}

void WsLink::close() {
    _rx.clear();
    _closing = true;
    drop();
}

void WsLink::drop() {
    _connected = false;
    _dropRequested = true;
}

bool WsLink::write(std::span<const std::byte> frame) {
    if (!_connected || frame.size() > valence::kFrameBufferCapacity) {
        _unsent.fetch_add(1);
        return false;
    }
    TxItem it;
    it.opcode = kOpBin;
    it.len = uint16_t(frame.size());
    std::memcpy(it.data, frame.data(), frame.size());
    if (xQueueSend(_tx, &it, 0) != pdPASS) {
        _unsent.fetch_add(1);
        return false;
    }
    return true;
}

valence::TransportProperties WsLink::properties() const {
    valence::TransportProperties p{};
    p.mtu = uint16_t(valence::kFrameBufferCapacity);
    p.ordered = true;
    p.reliable = true;
    p.congestion = valence::CongestionSignal::QueueWatermark;
    return p;
}

// ---- reader task: connection owner -------------------------------------------------

bool WsLink::readExact(int fd, uint8_t* p, size_t n) {
    while (n > 0) {
        if (_dropRequested) return false;  // checked every read: a busy hub never lets recv time out
        const ssize_t k = ::recv(fd, p, n, 0);
        if (k > 0) {
            p += k;
            n -= size_t(k);
            continue;
        }
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;  // poll tick
        return false;
    }
    return true;
}

bool WsLink::connectAndUpgrade(int fd) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(_port.load());
    a.sin_addr.s_addr = _ip.load();
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) return false;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    uint8_t nonce[16];
    esp_fill_random(nonce, sizeof(nonce));
    unsigned char key[32];
    size_t kn = 0;
    mbedtls_base64_encode(key, sizeof(key), &kn, nonce, sizeof(nonce));
    char req[256];
    const ip4_addr_t host{_ip.load()};
    const int rn = std::snprintf(req, sizeof(req),
                                 "GET / HTTP/1.1\r\nHost: %s:%u\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                 "Sec-WebSocket-Key: %.*s\r\nSec-WebSocket-Version: 13\r\n"
                                 "Sec-WebSocket-Protocol: valence.v1\r\n\r\n",
                                 ip4addr_ntoa(&host), unsigned(_port.load()), int(kn), key);
    if (!sendAll(fd, reinterpret_cast<const uint8_t*>(req), size_t(rn))) return false;

    // Read the response head byte by byte: the first frame may follow it at once.
    char head[512];
    size_t hn = 0;
    while (hn < sizeof(head) - 1) {
        if (!readExact(fd, reinterpret_cast<uint8_t*>(head + hn), 1)) return false;
        ++hn;
        if (hn >= 4 && std::memcmp(head + hn - 4, "\r\n\r\n", 4) == 0) break;
    }
    head[hn] = '\0';
    // SPEC §13.2: the server MUST echo valence.v1; a strict client fails without it.
    if (std::strncmp(head, "HTTP/1.1 101", 12) != 0 || !std::strstr(head, "valence.v1")) {
        ESP_LOGW(kTag, "upgrade refused: %.40s", head);
        return false;
    }
    return true;
}

void WsLink::readerTask() {
    auto* buf = static_cast<uint8_t*>(heap_caps_malloc(valence::kFrameBufferCapacity, MALLOC_CAP_INTERNAL));
    configASSERT(buf);
    uint32_t backoffMs = kReconnectMs;  // doubles while the hub refuses the socket
    for (;;) {
        if (_ip.load() == 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        _dropRequested = false;
        const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(kReconnectMs));
            continue;
        }
        setTimeout(fd, SO_RCVTIMEO, kRecvPollMs);
        setTimeout(fd, SO_SNDTIMEO, kSendTimeoutMs);
        if (!connectAndUpgrade(fd)) {
            ::close(fd);
            vTaskDelay(pdMS_TO_TICKS(backoffMs + esp_random() % (backoffMs / 4 + 1)));
            backoffMs = backoffMs * 2 > kReconnectMaxMs ? kReconnectMaxMs : backoffMs * 2;
            continue;
        }
        backoffMs = kReconnectMs;
        xQueueReset(_tx);
        _rx.clear();
        _fd = fd;
        _connected = true;
        ESP_LOGI(kTag, "connected");

        size_t msgLen = 0;     // bytes of the binary message being assembled
        bool msgTooBig = false;
        for (;;) {
            uint8_t h[2];
            if (!readExact(fd, h, 2)) break;
            const uint8_t op = h[0] & 0x0F;
            const bool fin = h[0] & 0x80;
            uint64_t len = h[1] & 0x7F;
            if (h[1] & 0x80) break;  // RFC 6455: a server frame is never masked
            if (len == 126) {
                uint8_t e[2];
                if (!readExact(fd, e, 2)) break;
                len = (uint64_t(e[0]) << 8) | e[1];
            } else if (len == 127) {
                uint8_t e[8];
                if (!readExact(fd, e, 8)) break;
                len = 0;
                for (uint8_t b : e) len = (len << 8) | b;
            }
            if (op == kOpBin || op == kOpCont) {
                if (op == kOpBin) {
                    msgLen = 0;
                    msgTooBig = false;
                }
                bool ok = true;
                while (len > 0 && ok) {  // copy what fits, discard the rest
                    uint8_t sink[64];
                    if (!msgTooBig && msgLen + len <= valence::kFrameBufferCapacity) {
                        ok = readExact(fd, buf + msgLen, size_t(len));
                        msgLen += size_t(len);
                        len = 0;
                    } else {
                        msgTooBig = true;
                        const size_t k = len < sizeof(sink) ? size_t(len) : sizeof(sink);
                        ok = readExact(fd, sink, k);
                        len -= k;
                    }
                }
                if (!ok) break;
                if (fin && !msgTooBig) {
                    _rx.push(std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf), msgLen));
                } else if (fin) {
                    ESP_LOGW(kTag, "dropped an oversized message");
                }
                continue;
            }
            if (len > 125) break;  // control frames are short; anything else is a protocol error
            uint8_t ctl[125];
            if (!readExact(fd, ctl, size_t(len))) break;
            if (op == kOpPing) sendControl(kOpPong, ctl, size_t(len));
            if (op == kOpClose || op == kOpText) {  // §13.2: text on valence.v1 is a protocol error
                sendControl(kOpClose, nullptr, 0);
                break;
            }
        }

        const bool asked = _closing.load();
        if (asked) {
            ESP_LOGI(kTag, "closed for a new session");
        } else {
            ESP_LOGW(kTag, "link down");
        }
        _connected = false;
        _fd = -1;
        ::shutdown(fd, SHUT_RDWR);  // unblocks a send in progress
        while (_txBusy.load()) vTaskDelay(pdMS_TO_TICKS(5));
        ::close(fd);
        _closing = false;
        vTaskDelay(pdMS_TO_TICKS(asked ? 200 : kReconnectMs));
    }
}

void WsLink::sendControl(uint8_t opcode, const uint8_t* p, size_t n) {
    TxItem it;
    it.opcode = opcode;
    it.len = uint16_t(n);
    if (n) std::memcpy(it.data, p, n);
    xQueueSend(_tx, &it, 0);  // full: a PONG or CLOSE is not worth blocking the reader
}

// ---- writer task: the only sender ---------------------------------------------------

void WsLink::writerTask() {
    auto* it = static_cast<TxItem*>(heap_caps_malloc(sizeof(TxItem), MALLOC_CAP_INTERNAL));
    auto* out = static_cast<uint8_t*>(heap_caps_malloc(valence::kFrameBufferCapacity + 14, MALLOC_CAP_INTERNAL));
    configASSERT(it && out);
    for (;;) {
        if (xQueueReceive(_tx, it, pdMS_TO_TICKS(kRecvPollMs)) != pdPASS) continue;
        const int fd = _fd.load();
        if (fd < 0 || !_connected) {
            if (it->opcode == kOpBin && !_closing) _unsent.fetch_add(1);
            continue;
        }
        size_t n = 0;
        out[n++] = uint8_t(0x80 | it->opcode);
        if (it->len < 126) {
            out[n++] = uint8_t(0x80 | it->len);
        } else {
            out[n++] = 0x80 | 126;
            out[n++] = uint8_t(it->len >> 8);
            out[n++] = uint8_t(it->len);
        }
        uint8_t mask[4];
        esp_fill_random(mask, sizeof(mask));
        std::memcpy(out + n, mask, 4);
        n += 4;
        for (size_t i = 0; i < it->len; ++i) out[n + i] = uint8_t(it->data[i]) ^ mask[i & 3];
        n += it->len;
        _txBusy = true;
        const bool ok = _connected && sendAll(fd, out, n);
        _txBusy = false;
        if (!ok) {
            if (it->opcode == kOpBin) _unsent.fetch_add(1);
            ESP_LOGW(kTag, "send failed (errno %d): dropping the link", errno);
            drop();
            ::shutdown(fd, SHUT_RDWR);
        }
    }
}

}  // namespace garnet
