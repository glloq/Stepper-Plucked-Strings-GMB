// Unit tests for the runtime telemetry accumulator (audit P2.19).
#include "TestFramework.h"
#include "../src/core/diagnostics/Diagnostics.h"

using namespace gmb;

TEST(diagnostics_counters_accumulate_and_latch) {
    Diagnostics d;
    d.addMidiEvents(3);
    d.addMidiEvents(2);
    CHECK_EQ((int)d.counters().midiEvents, 5);      // additive
    d.setMidiDropped(7);
    d.setMidiDropped(9);
    CHECK_EQ((int)d.counters().midiDropped, 9);      // latched (component total)
    d.setServoMoves(42);
    CHECK_EQ((int)d.counters().servoMoves, 42);
    d.setGovernorThrottles(4);
    CHECK_EQ((int)d.counters().governorThrottles, 4);
    d.addWifiReconnect();
    d.addWifiReconnect();
    CHECK_EQ((int)d.counters().wifiReconnects, 2);
}

TEST(diagnostics_cmd_queue_high_water_only_rises) {
    Diagnostics d;
    d.observeCmdQueueDepth(2);
    d.observeCmdQueueDepth(5);
    d.observeCmdQueueDepth(1);   // lower: must not lower the mark
    d.observeCmdQueueDepth(4);
    CHECK_EQ((int)d.counters().cmdQueueHighWater, 5);
}

TEST(diagnostics_scheduler_latency_and_jitter) {
    Diagnostics d;
    // First sample only seeds the mean (no jitter yet).
    d.observeSchedulerPeriodUs(1000);
    CHECK_EQ((int)d.counters().schedulerMaxLatencyUs, 1000);
    CHECK_EQ((int)d.counters().schedulerJitterUs, 0);
    CHECK_EQ((int)d.counters().schedulerMeanUs, 1000);
    // A steady period keeps jitter at 0 and the mean pinned.
    d.observeSchedulerPeriodUs(1000);
    CHECK_EQ((int)d.counters().schedulerJitterUs, 0);
    // A spike raises the max latency and records the deviation as jitter.
    d.observeSchedulerPeriodUs(5000);
    CHECK_EQ((int)d.counters().schedulerMaxLatencyUs, 5000);
    CHECK((int)d.counters().schedulerJitterUs >= 3900);  // ~5000-1000 minus EMA drift
    // The mean tracks upward but slowly (EMA alpha = 1/8), staying well below 5000.
    CHECK((int)d.counters().schedulerMeanUs > 1000);
    CHECK((int)d.counters().schedulerMeanUs < 2000);
}

TEST(diagnostics_pca_status_and_reset) {
    Diagnostics d;
    d.setPca(true, false, 10);
    CHECK(d.counters().pcaUsed);
    CHECK(!d.counters().pcaHealthy);
    CHECK_EQ((int)d.counters().pcaFailedBoard, 10);
    d.addMidiEvents(5);
    d.reset();
    CHECK_EQ((int)d.counters().midiEvents, 0);
    CHECK_EQ((int)d.counters().pcaFailedBoard, 0xFF);
    CHECK(d.counters().pcaHealthy);
    // After reset the scheduler mean re-seeds from the next sample.
    d.observeSchedulerPeriodUs(2000);
    CHECK_EQ((int)d.counters().schedulerMeanUs, 2000);
}

// Stepper build: the motion-side counters have no servo equivalent, so they are
// pinned separately. They are incremental (each event bumps them once) unlike the
// component totals that are latched.
TEST(diagnostics_motion_counters_are_incremental) {
    Diagnostics d;
    CHECK_EQ((int)d.counters().axisMoves, 0);
    d.setAxisMoves(17);                       // latched from the StepperBank total
    CHECK_EQ((int)d.counters().axisMoves, 17);
    d.addHomingFailure();
    d.addHomingFailure();
    CHECK_EQ((int)d.counters().homingFailures, 2);
    d.addLimitTrip();
    CHECK_EQ((int)d.counters().limitTrips, 1);
    d.addMoveTimeout();
    d.addMoveTimeout();
    d.addMoveTimeout();
    CHECK_EQ((int)d.counters().moveTimeouts, 3);
    d.setUdpRejected(5);
    CHECK_EQ((int)d.counters().udpRejected, 5);
    d.reset();
    CHECK_EQ((int)d.counters().axisMoves, 0);
    CHECK_EQ((int)d.counters().homingFailures, 0);
    CHECK_EQ((int)d.counters().limitTrips, 0);
    CHECK_EQ((int)d.counters().moveTimeouts, 0);
    CHECK_EQ((int)d.counters().udpRejected, 0);
}
