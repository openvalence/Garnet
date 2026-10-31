// The stroke window edited from two knobs (docs/m5-remote.md section 3).
// Header-only with no ESP-IDF include, so test/native proves it on the host.
// Constraints:
// - The result is always a legal window: lo <= min, min + floor <= max,
//   max <= hi. The hub refuses min >= max (config-set 0x3000) and a window
//   past max_rail, so no write may ever ask for either.
// - A size or position change moves both limits: it goes out as ONE frame
//   carrying both keys. Two frames would pass through a wrong size or an
//   inverted window between them.
// - Detents are applied to the target when they arrive and never dropped;
//   only the sends are paced (the channel's max rate).
#pragma once

#include <cstdint>

namespace garnet::window {

enum class Mode : uint8_t { RearFront, SizePosition };

struct Win {
    float min = 0.0f;  // rear limit, mm
    float max = 0.0f;  // front limit, mm
};

struct Limits {
    float lo = 0.0f;     // the rail's rear end
    float hi = 0.0f;     // the rail's front end (max_rail)
    float step = 1.0f;   // one detent on a limit
    float floor = 1.0f;  // the narrowest window
};

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// `w` moved inside the limits, its size kept where it fits.
inline Win normalize(Win w, const Limits& l) {
    w.max = clampf(w.max, l.lo + l.floor, l.hi);
    w.min = clampf(w.min, l.lo, w.max - l.floor);
    return w;
}

// Knob A and knob B by mode. Rear/Front: A moves the rear limit, B the
// front; a limit stops at the other (the floor apart), never pushes it.
// Size/Position: A grows or shrinks the window around its center by one
// step at each end per detent (pinned at a rail end, it keeps growing to
// the free side); B moves the whole window, stopping at a rail end with its
// size kept.
inline Win apply(Win w, Mode mode, int a, int b, const Limits& l) {
    if (l.hi - l.lo < l.floor) return w;
    w = normalize(w, l);
    if (mode == Mode::RearFront) {
        const float max = clampf(w.max + float(b) * l.step, w.min + l.floor, l.hi);
        const float min = clampf(w.min + float(a) * l.step, l.lo, max - l.floor);
        return {min, max};
    }
    const float size = clampf(w.max - w.min + 2.0f * float(a) * l.step, l.floor, l.hi - l.lo);
    const float center = (w.min + w.max) * 0.5f;
    Win r{center - size * 0.5f, center + size * 0.5f};
    if (r.min < l.lo) r = {l.lo, l.lo + size};
    if (r.max > l.hi) r = {l.hi - size, l.hi};
    const float d = clampf(float(b) * l.step, l.lo - r.min, l.hi - r.max);
    return {r.min + d, r.max + d};
}

// The two numbers the Home tab shows for a mode: rear and front, or size
// and position (the center).
inline Win shown(Win w, Mode mode) {
    if (mode == Mode::RearFront) return w;
    return {w.max - w.min, (w.min + w.max) * 0.5f};
}

// Knob detents to a target window, sent at most every gapMs (1000 / the
// channel's max rate) as the latest target.
struct Editor {
    Win target{};
    bool dirty = false;
    uint32_t lastSendMs = 0;
    bool sentOnce = false;

    void edit(Win base, Mode mode, int a, int b, const Limits& l) {
        target = apply(base, mode, a, b, l);
        dirty = true;
    }
    bool due(uint32_t nowMs, uint32_t gapMs) const {
        return dirty && (!sentOnce || nowMs - lastSendMs >= gapMs);
    }
    void sent(uint32_t nowMs) {
        dirty = false;
        sentOnce = true;
        lastSendMs = nowMs;
    }
};

}  // namespace garnet::window
