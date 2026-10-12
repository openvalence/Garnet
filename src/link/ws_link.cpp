#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/idf_additions.h"
#include "link/link.h"
#include "lwip/ip4_addr.h"

namespace garnet {
namespace {
constexpr const char* kTag = "ws";
constexpr TickType_t kSendTimeout = pdMS_TO_TICKS(20);

void wsEvent(void* arg, esp_event_base_t, int32_t id, void* data) { static_cast<WsLink*>(arg)->onEvent(id, data); }
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

// ---- WsLink -------------------------------------------------------------------

WsLink::WsLink()
    : _rx(96), _frag(static_cast<std::byte*>(heap_caps_malloc(valence::kFrameBufferCapacity, MALLOC_CAP_SPIRAM))) {}

void WsLink::start(uint32_t ipv4, uint16_t port) {
    stop();
    char uri[48];
    ip4_addr_t a{ipv4};
    std::snprintf(uri, sizeof(uri), "ws://%s:%u/", ip4addr_ntoa(&a), unsigned(port));
    esp_websocket_client_config_t c{};
    c.uri = uri;
    c.subprotocol = "valence.v1";
    c.buffer_size = 1024;
    c.task_stack = 6144;
    c.reconnect_timeout_ms = 2000;
    c.network_timeout_ms = 3000;
    c.ping_interval_sec = 0;  // Valence PING (§6.6) is the liveness check
    auto h = esp_websocket_client_init(&c);
    if (!h) return;
    esp_websocket_register_events(h, WEBSOCKET_EVENT_ANY, wsEvent, this);
    _client = h;
    ESP_LOGI(kTag, "connecting %s", uri);
    esp_websocket_client_start(h);
}

void WsLink::stop() {
    if (!_client) return;
    auto h = static_cast<esp_websocket_client_handle_t>(_client);
    esp_websocket_client_stop(h);
    esp_websocket_client_destroy(h);
    _client = nullptr;
    _connected = false;
    _rx.clear();
}

bool WsLink::write(std::span<const std::byte> frame) {
    if (!_client || !_connected) return false;
    const int n = esp_websocket_client_send_bin(static_cast<esp_websocket_client_handle_t>(_client),
                                                reinterpret_cast<const char*>(frame.data()), int(frame.size()),
                                                kSendTimeout);
    return n == int(frame.size());
}

valence::TransportProperties WsLink::properties() const {
    valence::TransportProperties p{};
    p.mtu = uint16_t(valence::kFrameBufferCapacity);
    p.ordered = true;
    p.reliable = true;
    p.congestion = valence::CongestionSignal::QueueWatermark;
    return p;
}

void WsLink::onEvent(int32_t id, void* data) {
    auto* d = static_cast<esp_websocket_event_data_t*>(data);
    switch (id) {
        case WEBSOCKET_EVENT_CONNECTED:
            _rx.clear();
            _connected = true;
            ESP_LOGI(kTag, "connected");
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
        case WEBSOCKET_EVENT_ERROR:
            if (_connected) ESP_LOGW(kTag, "link down (event %d)", int(id));
            _connected = false;
            break;
        case WEBSOCKET_EVENT_DATA: {
            if (d->op_code != 0x2 && d->op_code != 0x0) break;  // binary or its continuation
            if (d->payload_len <= d->data_len && d->payload_offset == 0) {
                _rx.push(std::span<const std::byte>(reinterpret_cast<const std::byte*>(d->data_ptr), size_t(d->data_len)));
                break;
            }
            const size_t end = size_t(d->payload_offset) + size_t(d->data_len);
            if (!_frag || end > valence::kFrameBufferCapacity) break;  // oversized: dropped
            std::memcpy(_frag + d->payload_offset, d->data_ptr, size_t(d->data_len));
            if (end == size_t(d->payload_len)) _rx.push(std::span<const std::byte>(_frag, end));
            break;
        }
        default:
            break;
    }
}

}  // namespace garnet
