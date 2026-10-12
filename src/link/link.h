// Link adapters: valence::ITransport over a real channel. The Client and
// everything that calls it run on the UI task; receive paths run on their own
// tasks (the WebSocket task, the console reader) and hand frames over through
// FrameQueue, the one cross-task boundary in Garnet.
#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <span>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "valence/transport/transport.hpp"

namespace garnet {

// Bounded hand-off of whole frames from a receive task to the UI task. Full
// means dropped: the Client repairs catalog gaps and STATE is newest-wins.
class FrameQueue {
public:
    explicit FrameQueue(size_t depth);
    bool push(std::span<const std::byte> frame);  // any task
    std::optional<valence::FrameBuffer> pop();    // UI task
    void clear();
    uint32_t dropped() const { return _dropped.load(); }

private:
    QueueHandle_t _q;
    std::atomic<uint32_t> _dropped{0};
};

// SPEC §13.2: one binary WS message per Valence frame, subprotocol valence.v1.
// The esp_websocket_client task reconnects on its own; open() reports whether
// the socket is up now, so the session calls Client::connect() only then.
class WsLink final : public valence::ITransport {
public:
    WsLink();
    void start(uint32_t ipv4, uint16_t port);  // network byte order ip
    void stop();                               // blocks while the WS task exits
    bool started() const { return _client != nullptr; }
    bool connected() const { return _connected.load(); }

    bool open() override { return connected(); }
    void close() override { _rx.clear(); }
    bool write(std::span<const std::byte> frame) override;
    std::optional<valence::FrameBuffer> read() override { return _rx.pop(); }
    valence::TransportProperties properties() const override;

    // WS task only.
    void onEvent(int32_t id, void* data);

private:
    void* _client = nullptr;  // esp_websocket_client_handle_t
    std::atomic<bool> _connected{false};
    FrameQueue _rx;
    std::byte* _frag = nullptr;  // reassembly of a fragmented message, WS task only
};

// Bench link over the USB console: frames travel as "@V <base64>" lines and
// tools/tether_bridge.py relays them to a hub's WebSocket. Shares the port with
// the log, which is why it is line-framed.
class TetherLink final : public valence::ITransport {
public:
    TetherLink();
    bool open() override { return true; }
    void close() override { _rx.clear(); }
    bool write(std::span<const std::byte> frame) override;
    std::optional<valence::FrameBuffer> read() override { return _rx.pop(); }
    valence::TransportProperties properties() const override;

    void pushLine(const char* b64);  // console task: one "@V" payload

private:
    FrameQueue _rx;
};

// SPEC §13.8 discovery probes and RFC-053 ESTOP datagrams on one non-blocking
// UDP socket. UI task only.
class Udp {
public:
    bool open();
    bool isOpen() const { return _fd >= 0; }
    void close();
    bool sendProbe(uint32_t broadcastIpv4);
    // Returns true and fills the reply when one arrived; call until false.
    bool pollReply(uint32_t& ipv4, uint16_t& wsPort, char* name, size_t nameCap, uint64_t& instanceId);
    bool sendTo(uint32_t ipv4, std::span<const std::byte> datagram);

private:
    int _fd = -1;
};

}  // namespace garnet
