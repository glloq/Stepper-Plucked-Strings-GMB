// P2.17: the BOOT-button long-press detector extracted from main.cpp.
#include "TestFramework.h"
#include "../src/core/util/HoldButton.h"

using namespace gmb;

TEST(holdbutton_fires_once_after_hold) {
    HoldButton b;
    b.configure(2000);
    CHECK(!b.update(true, 0));       // press edge
    CHECK(!b.update(true, 1000));    // still within the hold window
    CHECK(!b.update(true, 1999));    // just short
    CHECK(b.update(true, 2000));     // crosses 2000 ms -> fires ONCE
    CHECK(!b.update(true, 2500));    // held longer: no repeat
    CHECK(!b.update(true, 9000));    // still no repeat
}

TEST(holdbutton_rearms_on_release) {
    HoldButton c;
    c.configure(1000);
    c.update(true, 0);               // press edge
    CHECK(c.update(true, 1000));     // first fire
    CHECK(!c.update(false, 1100));   // released
    c.update(true, 1200);            // new press edge
    CHECK(!c.update(true, 2000));    // 800 ms held since the new edge: not yet
    CHECK(c.update(true, 2200));     // 1000 ms since the new edge -> fires again
}

TEST(holdbutton_short_press_never_fires) {
    HoldButton b;
    b.configure(2000);
    b.update(true, 0);
    CHECK(!b.update(true, 500));
    CHECK(!b.update(false, 600));    // released before the hold -> never fires
    CHECK(!b.update(false, 3000));
}
