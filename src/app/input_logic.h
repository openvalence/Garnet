// Board-independent input logic: button debounce, encoder detents, knob
// acceleration and send pacing, the hold rule of a key mapped to halt, the
// power key's edges as a level, and the mirror of a directional action.
// Header-only with no ESP-IDF include, so test/native proves it on the host.
// Constraints:
// - No allocation and no statics: every instance belongs to one input.
// - Times are milliseconds from one free-running u32 clock; wrap is handled.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace garnet::input {

// A level counts once it reads the same for kScans consecutive scans.
struct Debounce {
    static constexpr uint8_t kScans = 3;  // 30 ms at the 10 ms scan
    bool held = false;
    uint8_t streak = 0;
    bool update(bool raw) {
        if (raw == held) {
            streak = 0;
        } else if (++streak >= kScans) {
            held = raw;
            streak = 0;
        }
        return held;
    }
};

// Raw quadrature counts to whole detents; the remainder carries over.
struct Detents {
    int countsPerDetent = 2;
    int carry = 0;
    int take(int counts) {
        carry += counts;
        const int d = carry / countsPerDetent;
        carry -= d * countsPerDetent;
        return d;
    }
};

// Detents to setting steps: detents closer than kRampMs on one knob ramp the
// step up to kRampMax (the original remote's feel); a reversal starts over.
// Steps go out at most every kSendGapMs so a fast spin is one write, not 50.
struct Knob {
    static constexpr uint32_t kRampMs = 75;
    static constexpr int kRampMax = 8;
    static constexpr uint32_t kSendGapMs = 50;
    int pending = 0;
    int ramp = 1;
    int lastDir = 0;
    uint32_t lastDetentMs = 0;
    uint32_t lastSendMs = 0;
    bool sentOnce = false;

    void turn(int detents, uint32_t nowMs) {
        while (detents != 0) {
            const int dir = detents > 0 ? 1 : -1;
            if (dir == lastDir && nowMs - lastDetentMs <= kRampMs) {
                if (ramp < kRampMax) ++ramp;
            } else {
                ramp = 1;
            }
            lastDir = dir;
            lastDetentMs = nowMs;
            pending += dir * ramp;
            detents -= dir;
        }
    }
    int take(uint32_t nowMs) {
        if (pending == 0 || (sentOnce && nowMs - lastSendMs < kSendGapMs)) return 0;
        const int p = pending;
        pending = 0;
        lastSendMs = nowMs;
        sentOnce = true;
        return p;
    }
};

// A key mapped to halt: the press latches; a press that began after the
// latch and is held kReleaseHoldMs releases (the on-screen Halt's rule). The
// press that latched never counts toward the release, so a key held on past
// the latch (the power key held to power the remote off) leaves the stop on.
struct HaltHold {
    static constexpr uint32_t kReleaseHoldMs = 3000;  // RFC-085: release is a held press
    bool down = false;
    bool armed = false;
    bool sent = false;
    uint32_t pressMs = 0;

    // A press edge with the hub's latch as seen now; true = send halt.
    bool press(bool latched, uint32_t nowMs) {
        down = true;
        armed = latched;
        sent = false;
        pressMs = nowMs;
        return !latched;
    }
    // Every scan with the key's level; true = send release, once per press.
    bool hold(bool level, uint32_t nowMs) {
        if (!level) {
            down = armed = false;
            return false;
        }
        if (down && armed && !sent && nowMs - pressMs >= kReleaseHoldMs) {
            sent = true;
            return true;
        }
        return false;
    }
    bool releasing() const { return down && armed && !sent; }
    uint32_t heldMs(uint32_t nowMs) const { return down ? nowMs - pressMs : 0; }
};

// The AXP2101 power key as a level. The PMIC reports it only as latched
// edges (reg 0x49: bit 1 press, bit 0 release), read and cleared once per
// scan, so a press and a release can land in one read; each of those cases
// shows for one scan so bsp::buttons() never loses an edge.
struct PowerKey {
    bool held = false;
    uint8_t forced = 0;  // 1: high for one scan, then low; 2: low for one scan, then high

    void edges(bool press, bool release) {
        if (press && release) {
            forced = held ? 2 : 1;  // held: the release came first; otherwise the press did
        } else if (press) {
            held = true;
        } else if (release) {
            held = false;  // also a release with no press seen: the key held through boot
        }
    }
    bool level() {
        if (forced != 0) {
            const bool v = forced == 1;
            forced = 0;
            return v;
        }
        return held;
    }
};

// The opposite direction of a directional action ("speed+" -> "speed-",
// "inc:0123.4" -> "dec:0123.4"); empty when the action has no direction.
inline std::string mirror(std::string_view a) {
    if (a.size() > 4 && a.substr(0, 4) == "inc:") return "dec:" + std::string(a.substr(4));
    if (a.size() > 4 && a.substr(0, 4) == "dec:") return "inc:" + std::string(a.substr(4));
    if (a.size() > 1 && (a.back() == '+' || a.back() == '-')) {
        std::string m(a);
        m.back() = a.back() == '+' ? '-' : '+';
        return m;
    }
    return {};
}

}  // namespace garnet::input
