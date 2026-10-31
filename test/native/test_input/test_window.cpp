#include <doctest.h>

#include <initializer_list>

#include "app/window_logic.h"

using namespace garnet::window;

namespace {
const Limits kRail{0.0f, 500.0f, 1.0f, 1.0f};

bool legal(const Win& w, const Limits& l) { return w.min >= l.lo && w.min + l.floor <= w.max + 1e-4f && w.max <= l.hi; }
}  // namespace

TEST_CASE("rear/front: each knob moves its own limit") {
    const Win w{100.0f, 300.0f};
    Win r = apply(w, Mode::RearFront, 5, 0, kRail);
    CHECK(r.min == doctest::Approx(105.0f));
    CHECK(r.max == doctest::Approx(300.0f));
    r = apply(w, Mode::RearFront, 0, -7, kRail);
    CHECK(r.min == doctest::Approx(100.0f));
    CHECK(r.max == doctest::Approx(293.0f));
}

TEST_CASE("rear/front: stops at the rail ends and never crosses the other limit") {
    Win r = apply({3.0f, 300.0f}, Mode::RearFront, -10, 0, kRail);
    CHECK(r.min == doctest::Approx(0.0f));
    r = apply({100.0f, 495.0f}, Mode::RearFront, 0, 20, kRail);
    CHECK(r.max == doctest::Approx(500.0f));
    r = apply({100.0f, 110.0f}, Mode::RearFront, 50, 0, kRail);  // rear runs into front
    CHECK(r.min == doctest::Approx(109.0f));
    CHECK(r.max == doctest::Approx(110.0f));
    r = apply({100.0f, 110.0f}, Mode::RearFront, 0, -50, kRail);  // front runs into rear
    CHECK(r.max == doctest::Approx(101.0f));
    CHECK(r.min == doctest::Approx(100.0f));
    r = apply({100.0f, 110.0f}, Mode::RearFront, 50, -50, kRail);  // both at once
    CHECK(legal(r, kRail));
}

TEST_CASE("size/position: size grows and shrinks around the center") {
    Win r = apply({100.0f, 300.0f}, Mode::SizePosition, 10, 0, kRail);
    CHECK(r.min == doctest::Approx(90.0f));
    CHECK(r.max == doctest::Approx(310.0f));
    r = apply({100.0f, 300.0f}, Mode::SizePosition, -10, 0, kRail);
    CHECK(r.min == doctest::Approx(110.0f));
    CHECK(r.max == doctest::Approx(290.0f));
    CHECK(shown(r, Mode::SizePosition).min == doctest::Approx(180.0f));  // size
    CHECK(shown(r, Mode::SizePosition).max == doctest::Approx(200.0f));  // position: the center
}

TEST_CASE("size/position: the size floor holds and the window never inverts") {
    const Win r = apply({100.0f, 300.0f}, Mode::SizePosition, -1000, 0, kRail);
    CHECK(r.max - r.min == doctest::Approx(1.0f));
    CHECK((r.min + r.max) * 0.5f == doctest::Approx(200.0f));
    CHECK(legal(r, kRail));
}

TEST_CASE("size/position: size pinned at a rail end grows to the free side, capped at the rail") {
    Win r = apply({0.0f, 100.0f}, Mode::SizePosition, 10, 0, kRail);
    CHECK(r.min == doctest::Approx(0.0f));
    CHECK(r.max == doctest::Approx(120.0f));
    r = apply({400.0f, 500.0f}, Mode::SizePosition, 10, 0, kRail);
    CHECK(r.max == doctest::Approx(500.0f));
    CHECK(r.min == doctest::Approx(380.0f));
    r = apply({100.0f, 300.0f}, Mode::SizePosition, 10000, 0, kRail);
    CHECK(r.min == doctest::Approx(0.0f));
    CHECK(r.max == doctest::Approx(500.0f));
}

TEST_CASE("size/position: position moves both limits and stops at each rail end with the size kept") {
    Win r = apply({100.0f, 300.0f}, Mode::SizePosition, 0, 25, kRail);
    CHECK(r.min == doctest::Approx(125.0f));
    CHECK(r.max == doctest::Approx(325.0f));
    r = apply({100.0f, 300.0f}, Mode::SizePosition, 0, 1000, kRail);
    CHECK(r.max == doctest::Approx(500.0f));
    CHECK(r.max - r.min == doctest::Approx(200.0f));
    r = apply({100.0f, 300.0f}, Mode::SizePosition, 0, -1000, kRail);
    CHECK(r.min == doctest::Approx(0.0f));
    CHECK(r.max - r.min == doctest::Approx(200.0f));
}

TEST_CASE("a stale window past the rail comes back inside before it moves") {
    const Limits shortRail{0.0f, 250.0f, 1.0f, 1.0f};
    for (Mode m : {Mode::RearFront, Mode::SizePosition}) {
        CHECK(legal(apply({200.0f, 400.0f}, m, 0, 0, shortRail), shortRail));
        CHECK(legal(apply({300.0f, 400.0f}, m, 3, -3, shortRail), shortRail));
    }
}

TEST_CASE("editor: detents from both knobs coalesce to at most one send per 100 ms, none lost") {
    Editor e;
    Win hub{100.0f, 300.0f};
    Win sentWin{};
    int sends = 0;
    uint32_t lastSend = 0;
    // 60 ms of A (+1 every 10 ms) and B (-1 every 10 ms), then quiet.
    for (uint32_t t = 0; t <= 400; t += 10) {
        if (t < 60) {
            const Win base = e.dirty || e.sentOnce ? e.target : hub;
            e.edit(base, Mode::RearFront, 1, -1, kRail);
        }
        if (e.due(t, 100)) {
            if (sends) CHECK(t - lastSend >= 100);
            sentWin = e.target;
            lastSend = t;
            e.sent(t);
            ++sends;
        }
    }
    CHECK(sends == 2);  // the first detent at once, the rest together 100 ms later
    CHECK(sentWin.min == doctest::Approx(106.0f));
    CHECK(sentWin.max == doctest::Approx(294.0f));
    CHECK_FALSE(e.dirty);
}

TEST_CASE("editor: a mode change between sends keeps the detents already taken") {
    Editor e;
    e.edit({100.0f, 300.0f}, Mode::RearFront, 4, 0, kRail);  // rear +4
    CHECK(e.due(0, 100));
    e.sent(0);
    e.edit(e.target, Mode::SizePosition, 0, 10, kRail);  // then position +10
    CHECK_FALSE(e.due(50, 100));
    CHECK(e.due(100, 100));
    CHECK(e.target.min == doctest::Approx(114.0f));
    CHECK(e.target.max == doctest::Approx(310.0f));
}
