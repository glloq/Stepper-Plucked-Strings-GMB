#include "TestFramework.h"
#include "../src/core/instrument/ServoActivationGovernor.h"

using namespace gmb;

// staggerMs == 0 disables throttling: every request is granted.
TEST(governor_disabled_when_stagger_zero) {
    ServoActivationGovernor g;
    g.configure(1, 0);
    for (int i = 0; i < 10; ++i) CHECK(g.requestStart(0));
}

// maxConcurrent == 1 spaces starts by staggerMs.
TEST(governor_serialises_single_slot) {
    ServoActivationGovernor g;
    g.configure(1, 10);
    CHECK(g.requestStart(1000));    // first: granted
    CHECK(!g.requestStart(1000));   // same instant: denied
    CHECK(!g.requestStart(1005));   // 5 ms later: still within stagger
    CHECK(g.requestStart(1010));    // 10 ms later: granted
    CHECK(!g.requestStart(1011));   // immediately after: denied again
}

// maxConcurrent == 3 lets three starts fire together, then throttles the burst.
TEST(governor_caps_concurrent_burst) {
    ServoActivationGovernor g;
    g.configure(3, 8);
    CHECK(g.requestStart(500));     // 1
    CHECK(g.requestStart(500));     // 2
    CHECK(g.requestStart(500));     // 3 — window now full
    CHECK(!g.requestStart(500));    // 4th in the same 8 ms window: denied
    CHECK(!g.requestStart(507));    // still inside the window relative to #1
    // 8 ms after the burst all three starts have aged out of the window, so three
    // fresh permits are available again — then it is full once more.
    CHECK(g.requestStart(508));
    CHECK(g.requestStart(508));
    CHECK(g.requestStart(508));
    CHECK(!g.requestStart(508));
}

// reset() clears the recent-starts history.
TEST(governor_reset_clears_history) {
    ServoActivationGovernor g;
    g.configure(1, 100);
    CHECK(g.requestStart(0));
    CHECK(!g.requestStart(1));
    g.reset();
    CHECK(g.requestStart(1));       // history cleared: granted again
}

// P2.19: the throttle counter counts every deferred start and, unlike the windows,
// is NOT cleared by reset() (it is cumulative telemetry).
TEST(governor_throttle_count_is_cumulative) {
    ServoActivationGovernor g;
    g.configure(1, 100);
    CHECK_EQ((int)g.throttleCount(), 0);
    CHECK(g.requestStart(0));        // granted -> no throttle
    CHECK(!g.requestStart(1));       // denied -> throttle #1
    CHECK(!g.requestStart(2));       // denied -> throttle #2
    CHECK_EQ((int)g.throttleCount(), 2);
    g.reset();
    CHECK_EQ((int)g.throttleCount(), 2);  // reset clears windows, not the counter
    CHECK(g.requestStart(3));        // granted after reset
    CHECK(!g.requestStart(4));       // denied -> throttle #3
    CHECK_EQ((int)g.throttleCount(), 3);
    // Throttling disabled (staggerMs 0) never counts.
    ServoActivationGovernor off;
    off.configure(1, 0);
    for (int i = 0; i < 5; ++i) off.requestStart(0);
    CHECK_EQ((int)off.throttleCount(), 0);
}

// A very large burst is fanned out over time rather than all-at-once: across a
// window only maxConcurrent permits are ever granted.
TEST(governor_fans_out_large_chord) {
    ServoActivationGovernor g;
    g.configure(2, 5);
    int granted = 0;
    for (int i = 0; i < 6; ++i) if (g.requestStart(2000)) ++granted;
    CHECK_EQ(granted, 2);           // only 2 of 6 at the same instant
    // Advancing past the stagger frees the slots again.
    granted = 0;
    for (int i = 0; i < 6; ++i) if (g.requestStart(2005)) ++granted;
    CHECK_EQ(granted, 2);
}

// A global cap of 0 means "no limit": every request is granted (even with a stagger).
TEST(governor_global_cap_zero_is_unlimited) {
    ServoActivationGovernor g;
    g.configure(/*global*/0, /*stagger*/10);
    for (int i = 0; i < 20; ++i) CHECK(g.requestStart(1000));  // all at the same instant
}

// The per-PCA-board cap throttles one board without touching the others.
TEST(governor_per_board_cap_is_independent) {
    ServoActivationGovernor g;
    g.configure(/*global*/0, /*perBoard*/1, /*stagger*/10);
    CHECK(g.requestStart(1000, /*board*/0));    // board 0: granted
    CHECK(!g.requestStart(1000, /*board*/0));   // board 0 again, same instant: denied
    CHECK(g.requestStart(1000, /*board*/1));    // a different board is unaffected
    CHECK(g.requestStart(1000, /*board*/2));
    CHECK(g.requestStart(1010, /*board*/0));    // board 0 frees after the stagger
}

// A servo on no board (0xFF, e.g. direct GPIO) is bounded by the global cap only.
TEST(governor_boardless_servo_uses_global_only) {
    ServoActivationGovernor g;
    g.configure(/*global*/1, /*perBoard*/1, /*stagger*/10);
    CHECK(g.requestStart(1000, 0xFF));          // global slot: granted
    CHECK(!g.requestStart(1000, 0xFF));         // global full: denied
}

// Global and per-board caps are enforced together: a free board is still denied when
// the whole-instrument cap is already spent.
TEST(governor_global_and_per_board_both_apply) {
    ServoActivationGovernor g;
    g.configure(/*global*/2, /*perBoard*/1, /*stagger*/10);
    CHECK(g.requestStart(1000, /*board*/0));    // global 1 / board 0
    CHECK(g.requestStart(1000, /*board*/1));    // global 2 / board 1 — global now full
    CHECK(!g.requestStart(1000, /*board*/2));   // board 2 free, but the global cap is hit
}

// Forecast (audit 4 P1.3): nextSlotDelayMs reports how long a NEW start must wait
// given the current window state, without recording anything.
TEST(governor_next_slot_delay_forecast) {
    ServoActivationGovernor g;
    g.configure(/*global*/1, /*perBoard*/1, /*stagger*/50);
    CHECK_EQ((int)g.nextSlotDelayMs(1000, 0), 0);   // empty windows: immediate
    CHECK(g.requestStart(1000, /*board*/0));
    // Global window full: a new start anywhere waits out the stagger.
    CHECK_EQ((int)g.nextSlotDelayMs(1001, 0xFF), 49);
    CHECK_EQ((int)g.nextSlotDelayMs(1010, 0), 40);
    CHECK_EQ((int)g.nextSlotDelayMs(1050, 0), 0);   // window elapsed
    // Forecasting must not consume a slot: the real grant still succeeds.
    CHECK(g.requestStart(1050, /*board*/0));
}

// The per-board window bounds the forecast even when the global window has room.
TEST(governor_next_slot_delay_per_board) {
    ServoActivationGovernor g;
    g.configure(/*global*/8, /*perBoard*/1, /*stagger*/40);
    CHECK(g.requestStart(2000, /*board*/3));
    CHECK_EQ((int)g.nextSlotDelayMs(2010, 3), 30);    // board 3 window still busy
    CHECK_EQ((int)g.nextSlotDelayMs(2010, 4), 0);     // another board: free
    CHECK_EQ((int)g.nextSlotDelayMs(2010, 0xFF), 0);  // direct GPIO: global only
}

// Full batch forecast (audit 5): a PARTIALLY occupied sliding window — historical
// grants expiring at different instants — is simulated exactly. The audit's
// example: cap 3 / stagger 10, grants at t-7 and t-1, then 6 new starts: the last
// one can only begin 19 ms after now (not the ~10 ms a closed-form batch estimate
// would claim).
TEST(governor_forecast_batch_with_partial_history) {
    ServoActivationGovernor g;
    g.configure(/*global*/3, /*perBoard*/0, /*stagger*/10);
    CHECK(g.requestStart(93, 0));
    CHECK(g.requestStart(99, 0));
    uint8_t boards[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    CHECK_EQ((int)g.forecastBatchDelayMs(100, boards, 6), 19);
    // The forecast recorded nothing: the real grants still follow the same path.
    CHECK(g.requestStart(100, 0));    // third slot of the window
    CHECK(!g.requestStart(100, 0));   // full until 93+10
}

// The batch forecast honours the per-board windows independently (audit 5).
TEST(governor_forecast_batch_per_board) {
    ServoActivationGovernor g;
    g.configure(/*global*/0, /*perBoard*/1, /*stagger*/50);
    // Two starts on DIFFERENT boards: no cross-blocking, both immediate.
    uint8_t spread[2] = {0, 1};
    CHECK_EQ((int)g.forecastBatchDelayMs(1000, spread, 2), 0);
    // Two starts on the SAME board: the second waits out the board window.
    uint8_t same[2] = {2, 2};
    CHECK_EQ((int)g.forecastBatchDelayMs(1000, same, 2), 50);
}
