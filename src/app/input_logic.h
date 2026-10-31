// Board-independent input logic: button debounce, encoder detents, knob
// acceleration and send pacing, and the mirror of a directional action.
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
