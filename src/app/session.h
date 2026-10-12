// Session: owns the valence::Client, the links, discovery, pairing state and
// the catalog model, and turns user actions into protocol calls.
// Constraints:
// - UI task only, every call. The Client is not thread-safe and needs none:
//   receive tasks reach it only through FrameQueue (link/link.h).
// - Halt must work in every state: in a session it rides the Client (raw
//   frame repeated until the latch is seen, plus the estop op when LIVE);
//   without a LIVE session it also goes out as the RFC-053 datagram.
// - resume and release are sent only from a deliberate user action, never
//   from Garnet's own logic (SPEC §11.1).
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "app/catalog_model.h"
#include "link/link.h"
#include "valence/client/client.hpp"
#include "valence/client/estop_datagram.hpp"

namespace garnet {

enum class LinkMode : uint8_t { Wifi = 0, Tether = 1 };

class Session final : public valence::ClientDelegate {
public:
    void begin();               // NVS identity, model, client
    void tick(uint32_t nowUs);  // every ~10 ms

    LinkMode mode() const { return _mode; }
    void setMode(LinkMode m);   // persisted
    void connectTo(uint32_t ipv4, uint16_t port);  // a typed hub address (console)
    void forgetHub();

    valence::ClientSessionState state() const { return _client->state(); }
    valence::AccessLevel roles() const { return _client->roles(); }
    bool live() const { return state() == valence::ClientSessionState::LIVE; }
    bool linkStale() const;
    bool estopLatched() const;
    bool pauseLatched() const { return _client->pauseLatched(); }
    bool estopCutsPower() const { return _client->estopCutsPower(); }
    bool haltUnconfirmed() const;  // a halt went out and no latch has been seen
    bool paired() const { return _id.hasToken; }
    const char* hubName() const { return _hubName; }
    const char* status() const { return _status; }
    // Total frames and writes lost to the link, while the last loss is recent
    // (the strip shows it); 0 otherwise.
    uint32_t lostRecently() const;
    // Bench: block the UI task for `ms` once the next catalog transfer starts,
    // to prove the link keeps reading while the consumer is stalled.
    void stallOnSync(uint32_t ms) { _stallOnSyncMs = ms; }
    void printLinkStats() const;
    Model& model() { return *_model; }
    TetherLink& tether() { return _tether; }

    void halt();
    void pauseToggle();
    void releaseHalt();
    void knock();
    bool write(const Setting& s, float physical);
    bool nudge(const Setting& s, int dir);
    bool toggle(const Setting& s);
    // A mapping action that is not UI navigation; false when unknown.
    bool runAction(std::string_view action);

    // ClientDelegate
    void onStateChange(valence::ClientSessionState s) override;
    void onState(uint16_t ch, uint16_t seq, std::span<const std::byte> payload) override;
    void onEcho(uint16_t, const valence::IntentValueMap&, uint16_t) override {}
    void onNack(const valence::NackMsg& n) override;
    void onPendingDropped(uint16_t) override { ++_writesLost; }
    void onPairGrant(std::span<const std::byte> token, valence::AccessLevel roles) override;
    void onCatalog(std::span<const std::byte> encoded, bool verified) override;

private:
    struct EspClock final : valence::IClock {
        uint32_t nowUs() const override;
    };
    struct EspRandom final : valence::IRandom {
        uint32_t nextU32() override;
        void fill(std::span<std::byte> out) override;
    };
    static bool sendDatagram(void* ctx, std::span<const std::byte> d);

    void buildClient();
    void tickWifi(uint32_t nowMs);
    void setStatus(const char* fmt, ...);
    // Leaves the hub (session and socket) and comes back after a jittered,
    // doubling delay of at least `atLeastMs` (a NACK's retry_after_ms).
    void scheduleRetry(uint32_t atLeastMs, const char* why);
    float current(const Setting& s);

    LinkMode _mode = LinkMode::Wifi;
    EspClock _clock;
    EspRandom _rng;
    valence::ClientIdentity _id{};
    char _name[24] = "Garnet";
    valence::Client* _client = nullptr;  // PSRAM, rebuilt on mode or token change
    Model* _model = nullptr;             // PSRAM
    WsLink _ws;
    TetherLink _tether;
    Udp _udp;
    valence::EstopDatagramSender _datagram{&Session::sendDatagram, this, 0};

    uint32_t _nowMs = 0;
    uint32_t _hubIp = 0;     // a typed address (persisted); 0 = discover
    uint32_t _activeIp = 0;  // the hub the link targets now
    uint16_t _hubPort = 0;
    uint64_t _savedHubId = 0;
    char _hubName[33] = "";
    char _status[64] = "starting";
    uint32_t _lastProbeMs = 0;
    uint32_t _retryAtMs = 0;
    uint32_t _connectStartMs = 0;
    bool _needSubscribe = false;
    bool _rebuild = false;
    bool _haltPending = false;
    uint32_t _haltAtMs = 0;
    // Health (no silent loss): frames the link could not send, intents that
    // died unanswered with a session. Shown on the strip for kLossShowMs.
    uint32_t _writesLost = 0;
    uint32_t _stallOnSyncMs = 0;
    uint32_t _backoffMs = 0;  // 0 after LIVE; doubles per failed attempt
    bool _busyRetry = false;
    uint32_t _busyRetryMs = 0;
    uint32_t _lossSeen = 0;
    uint32_t _lossAtMs = 0;

    // A nudge between a write and its STATE builds on what was asked, not on
    // the stale shown value.
    const Setting* _lastWrite = nullptr;
    float _lastWriteValue = 0.0f;
    uint32_t _lastWriteMs = 0;
};

}  // namespace garnet
