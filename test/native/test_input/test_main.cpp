#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "app/input_logic.h"

using namespace garnet::input;

TEST_CASE("debounce: a level counts after three equal scans, bounce never does") {
    Debounce d;
    CHECK_FALSE(d.update(true));
    CHECK_FALSE(d.update(false));  // bounce resets the streak
    CHECK_FALSE(d.update(true));
    CHECK_FALSE(d.update(true));
    CHECK(d.update(true));
    CHECK(d.update(false));  // release needs three scans too
    CHECK(d.update(false));
    CHECK_FALSE(d.update(false));
}

TEST_CASE("detents: two counts per detent, the remainder carries, both directions") {
    Detents t;
    CHECK(t.take(1) == 0);
    CHECK(t.take(1) == 1);
    CHECK(t.take(5) == 2);
    CHECK(t.take(-1) == 0);  // carry 1 - 1
    CHECK(t.take(-4) == -2);
}

TEST_CASE("knob: slow detents are one step each, a fast spin ramps to the cap") {
    Knob k;
    k.turn(1, 1000);
    CHECK(k.take(1000) == 1);
    k.turn(1, 1200);  // slower than the ramp window
    CHECK(k.take(1200) == 1);
    k.turn(10, 1250);  // ten detents inside one window: 2, 3, ... 8, 8, 8
    CHECK(k.take(1250) == 2 + 3 + 4 + 5 + 6 + 7 + 8 + 8 + 8 + 8);
}

TEST_CASE("knob: a reversal restarts the ramp, sends are paced") {
    Knob k;
    k.turn(3, 0);
    CHECK(k.take(0) == 1 + 2 + 3);
    k.turn(-1, 10);
    CHECK(k.take(10) == 0);  // inside the send gap: held
    CHECK(k.take(60) == -1);
    CHECK(k.take(200) == 0);
}

TEST_CASE("mirror: directional actions flip, the rest have no mirror") {
    CHECK(mirror("speed+") == "speed-");
    CHECK(mirror("menu-") == "menu+");
    CHECK(mirror("inc:00a1.3") == "dec:00a1.3");
    CHECK(mirror("dec:00a1.3") == "inc:00a1.3");
    CHECK(mirror("halt").empty());
    CHECK(mirror("toggle:00a1.3").empty());
    CHECK(mirror("run").empty());
}

TEST_CASE("halt hold: the press latches, a new press held 3 s releases once") {
    HaltHold h;
    CHECK(h.press(false, 0));  // not latched: this press sends halt
    CHECK_FALSE(h.hold(true, 3500));  // the latching press never releases
    CHECK_FALSE(h.hold(false, 3600));
    CHECK_FALSE(h.press(true, 4000));  // latched now: no second halt
    CHECK_FALSE(h.hold(true, 6999));
    CHECK(h.hold(true, 7000));
    CHECK_FALSE(h.hold(true, 9000));  // once per press
}

TEST_CASE("halt hold: the power key held to the AXP off time keeps the stop on") {
    HaltHold h;
    CHECK(h.press(false, 100));  // the press latches the halt
    for (uint32_t t = 110; t <= 10100; t += 10) CHECK_FALSE(h.hold(true, t));  // 10 s OFFLEVEL: the remote dies held
    CHECK_FALSE(h.releasing());
}

TEST_CASE("halt hold: letting go before 3 s cancels the release") {
    HaltHold h;
    h.press(true, 0);
    CHECK(h.releasing());
    CHECK_FALSE(h.hold(true, 2000));
    CHECK_FALSE(h.hold(false, 2010));
    CHECK_FALSE(h.hold(true, 5000));  // no new press edge: not armed
}

TEST_CASE("power key: edges rebuild the level") {
    PowerKey k;
    CHECK_FALSE(k.level());
    k.edges(true, false);
    CHECK(k.level());
    CHECK(k.level());  // held between reads
    k.edges(false, true);
    CHECK_FALSE(k.level());
    k.edges(false, true);  // a release with no press seen (held through boot)
    CHECK_FALSE(k.level());
}

TEST_CASE("power key: a press and its release in one read show for one scan") {
    PowerKey k;
    k.edges(true, true);
    CHECK(k.level());
    CHECK_FALSE(k.level());
}

TEST_CASE("power key: a release and a new press in one read show a gap") {
    PowerKey k;
    k.edges(true, false);
    CHECK(k.level());
    k.edges(true, true);
    CHECK_FALSE(k.level());  // the gap: the UI sees a new press edge next scan
    CHECK(k.level());
}

TEST_CASE("power key into halt hold: a tap inside one read still latches") {
    PowerKey k;
    HaltHold h;
    bool last = false;
    int halts = 0;
    k.edges(true, true);
    for (uint32_t t = 0; t < 50; t += 10) {
        const bool lv = k.level();
        if (lv && !last && h.press(false, t)) ++halts;
        h.hold(lv, t);
        last = lv;
    }
    CHECK(halts == 1);
}
