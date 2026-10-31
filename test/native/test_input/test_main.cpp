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
