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

// SPEC §13.2: one binary WS message per Valence frame, subprotocol valence.v1,
// on a plain lwIP socket. Two tasks, no shared lock:
// - the reader task owns the connection (connect, upgrade, reconnect) and
//   reads frames into the RX queue; it never waits on a send;
// - the writer task drains the TX queue into the socket. Only it sends.
// write() only enqueues, so the UI task never blocks on the network. A frame
// that cannot be queued or sent is counted in unsent(), never dropped
// silently (the session shows the count).
// Constraint: a hub stops serving a client that stops reading (Nucleus drops
// a control slot stalled 2 s), which is why receive must never wait on send.
class WsLink final : public valence::ITransport {
public:
    WsLink();
    void start(uint32_t ipv4, uint16_t port);  // network byte order ip; (re)targets the link
    void stop();                               // drops the connection and forgets the target
    bool started() const { return _ip.load() != 0; }
    bool connected() const { return _connected.load(); }
    uint32_t unsent() const { return _unsent.load(); }
    uint32_t rxDropped() const { return _rx.dropped(); }

    bool open() override { return connected(); }
    // Ends this socket: the next Client session gets a fresh one.
    void close() override;
    bool write(std::span<const std::byte> frame) override;
    std::optional<valence::FrameBuffer> read() override { return _rx.pop(); }
    valence::TransportProperties properties() const override;

    void readerTask();  // task bodies, public for the task trampolines
    void writerTask();

private:
    bool connectAndUpgrade(int fd);
    bool readExact(int fd, uint8_t* p, size_t n);
    void sendControl(uint8_t opcode, const uint8_t* p, size_t n);
    void drop();

    std::atomic<uint32_t> _ip{0};
    std::atomic<uint16_t> _port{0};
    std::atomic<int> _fd{-1};
    std::atomic<bool> _connected{false};
    std::atomic<bool> _dropRequested{false};
    std::atomic<bool> _txBusy{false};
    std::atomic<uint32_t> _unsent{0};
    FrameQueue _rx;
    QueueHandle_t _tx;  // TxItem, PSRAM
    bool _tasksStarted = false;
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
