// Regression tests for the P0 boot-safe + Homing/Arming state machine.
//
// The boot decision itself lives in main.cpp (Arduino-gated), but every invariant it
// relies on is expressed on the host-testable SafetyManager: no actuator may move
// unless the state is Armed, ConfigSafe latches with no valid profile, and Armed is
// reachable only through the Homing phase (which itself gates the carriage seek
// behind the pre-seek finger release).
#include "TestFramework.h"
#include "../src/core/configuration/Profile.h"
#include "../src/core/configuration/ProfileValidator.h"
#include "../src/core/instrument/InstrumentController.h"
#include "../src/core/safety/EstopPolarity.h"
#include "../src/core/safety/SafetyManager.h"

using namespace gmb;

// --- P0.1: the CONFIG_SAFE empty profile can never arm, and loads without crashing.
// This is the core guarantee behind boot-safe: with no valid profile the runtime
// holds an EMPTY Profile, which the validator rejects (so arming refuses) and which
// the instrument loads as zero strings (so nothing is ever driven).
TEST(empty_config_safe_profile_is_never_activatable) {
    Profile empty;                                   // what main.cpp holds in CONFIG_SAFE
    CHECK(!ProfileValidator::isActivatable(empty));  // can never be armed
    InstrumentController ic;
    ic.load(empty);                                  // must not crash / touch actuators
    CHECK_EQ((int)ic.stringCount(), 0);
    CHECK_EQ(ic.soundingCount(), 0);
}

// --- P0.1: no valid profile => ConfigSafe, actuators locked, cannot arm ---

TEST(configsafe_locks_actuators_until_a_profile_is_loaded) {
    SafetyManager s;
    s.boot();
    CHECK(s.state() == SafetyState::PowerOnSafe);
    CHECK(!s.actuatorsAllowed());          // nothing moves at boot

    s.configSafe();                        // no valid profile at boot
    CHECK(s.state() == SafetyState::ConfigSafe);
    CHECK(!s.actuatorsAllowed());          // and still nothing moves

    // A machine in ConfigSafe must NOT be armable directly — the empty/invalid
    // profile has to be replaced by a valid one (which goes through reset()).
    CHECK(!s.beginHoming(true, true, 100, 0));
    CHECK(!s.arm(true, true));
    CHECK(s.state() == SafetyState::ConfigSafe);

    // Loading a valid profile clears ConfigSafe (reset -> PowerOnSafe) and only THEN
    // can homing begin.
    s.reset();
    CHECK(s.state() == SafetyState::PowerOnSafe);
    CHECK(s.beginHoming(true, true, 100, 0));
    CHECK(s.state() == SafetyState::Homing);
}

// --- P0.2: Homing blocks play; the carriage seek waits for the finger release ---

TEST(homing_blocks_play_and_gates_the_seek_on_the_finger_release) {
    SafetyManager s;
    s.boot();
    CHECK(s.beginHoming(true, true, 250, 1000));  // 250 ms finger lift, started at t=1000
    CHECK(s.homing());
    CHECK(!s.actuatorsAllowed());                 // no MIDI note / test during Homing

    // Before the release elapses: no carriage may move yet.
    CHECK(!s.releaseComplete(1100));
    CHECK_EQ((int)s.releaseRemainingMs(1100), 150);
    CHECK(!s.actuatorsAllowed());

    // At the deadline the seek may start — but the machine is still NOT armed:
    // homing is sensor-driven, so only armAfterHoming() completes the sequence.
    CHECK(s.releaseComplete(1250));
    CHECK_EQ((int)s.releaseRemainingMs(1250), 0);
    CHECK(s.state() == SafetyState::Homing);
    CHECK(!s.actuatorsAllowed());

    CHECK(s.armAfterHoming());
    CHECK(s.state() == SafetyState::Armed);
    CHECK(s.actuatorsAllowed());                  // play allowed only now
    CHECK(!s.armAfterHoming());                   // no second transition
    CHECK_EQ((int)s.releaseRemainingMs(1300), 0); // no release window outside Homing
    CHECK(!s.releaseComplete(9999));
}

// A profile with no finger servo still starts its seek on the first tick (no
// negative wait / underflow).
TEST(homing_zero_release_wait_allows_the_seek_immediately) {
    SafetyManager s;
    s.boot();
    CHECK(s.beginHoming(true, true, 0, 5000));
    CHECK(s.releaseComplete(5000));
    CHECK(s.armAfterHoming());
    CHECK(s.state() == SafetyState::Armed);
}

// A re-home requested while already Homing restarts the release window instead of
// being refused (a profile activation can re-enter homing).
TEST(homing_can_restart_from_homing) {
    SafetyManager s;
    s.boot();
    CHECK(s.beginHoming(true, true, 100, 1000));
    CHECK(s.releaseComplete(1100));
    CHECK(s.beginHoming(true, true, 300, 2000));  // restart with a longer wait
    CHECK(!s.releaseComplete(2100));
    CHECK(s.releaseComplete(2300));
}

// --- P0.2: homing refuses an invalid profile / pins ---

TEST(homing_refuses_invalid_profile_or_pins) {
    SafetyManager s;
    s.boot();
    CHECK(!s.beginHoming(false, true, 100, 0));   // invalid profile
    CHECK(s.state() == SafetyState::PowerOnSafe);
    CHECK(!s.beginHoming(true, false, 100, 0));   // invalid pins
    CHECK(s.state() == SafetyState::PowerOnSafe);
    CHECK(!s.actuatorsAllowed());
}

// --- P0.3 / §21: a panic or E-stop latches and blocks arming until reset ---

TEST(panic_and_estop_latch_and_block_arming) {
    SafetyManager s;
    s.boot();
    s.beginHoming(true, true, 100, 0);
    s.armAfterHoming();
    CHECK(s.state() == SafetyState::Armed);

    s.panic("test", 2000);
    CHECK(s.state() == SafetyState::Panic);
    CHECK(!s.actuatorsAllowed());
    CHECK(!s.beginHoming(true, true, 100, 2000));  // cannot arm through a latched panic
    CHECK(!s.arm(true, true));
    CHECK(!s.armAfterHoming());

    s.reset();                                     // explicit recovery
    CHECK(s.state() == SafetyState::PowerOnSafe);
    CHECK(s.beginHoming(true, true, 100, 3000));   // arming allowed again

    // E-stop likewise latches from any state.
    s.emergencyStop(4000);
    CHECK(s.state() == SafetyState::EmergencyStop);
    CHECK(!s.actuatorsAllowed());
    CHECK(!s.beginHoming(true, true, 100, 4000));
}

// The whole point: actuatorsAllowed() is true in EXACTLY one state (Armed) and
// false in every safe/latched state, so nothing moves before Ready.
TEST(actuators_allowed_only_when_armed) {
    SafetyManager s;
    s.configSafe();   CHECK(!s.actuatorsAllowed());
    s.boot();         CHECK(!s.actuatorsAllowed());  // PowerOnSafe
    s.beginHoming(true, true, 100, 0);
    CHECK(!s.actuatorsAllowed());                    // Homing
    s.armAfterHoming();
    CHECK(s.actuatorsAllowed());                     // Armed — the only one
    s.panic("x", 0);  CHECK(!s.actuatorsAllowed());
    s.reset();        CHECK(!s.actuatorsAllowed());
    s.emergencyStop(0); CHECK(!s.actuatorsAllowed());
}

// The cumulative fault total feeds /api/diagnostics and must survive clearFaults()
// (which only trims the bounded, human-readable log).
TEST(fault_count_is_cumulative_across_clear) {
    SafetyManager s;
    s.boot();
    CHECK_EQ((int)s.faultCount(), 0);
    s.recordFault("axis", "LIMIT tripped", 10);
    s.recordFault("servo", "write failed", 20);
    CHECK_EQ((int)s.faultCount(), 2);
    CHECK_EQ((int)s.faults().size(), 2);
    s.clearFaults();
    CHECK_EQ((int)s.faults().size(), 0);
    CHECK_EQ((int)s.faultCount(), 2);   // total survives
    s.panic("boom", 30);                // panic records a fault too
    CHECK_EQ((int)s.faultCount(), 3);
}

// ---------------------------------------------------------------------------
// E-stop pin polarity. This is one line of arithmetic, and it is here because
// duplicating it is what broke it: estopAsserted() was correct in the continuous
// supervision while beginHoming() and doReset() each kept a `digitalRead(pin) ==
// LOW` test predating normally-closed support. On the RECOMMENDED normally-closed
// chain a healthy loop reads LOW, so those pre-arm checks refused to home a safe
// machine and passed a genuinely pressed E-stop. Every call site normalises
// through estopAssertedFor() now, and the whole truth table is pinned here.
// ---------------------------------------------------------------------------

TEST(estop_polarity_truth_table) {
    // Normally OPEN (legacy button): the button shorts the pin to GND.
    CHECK(!estopAssertedFor(true, false));   // HIGH (pull-up) = released -> run
    CHECK(estopAssertedFor(false, false));   // LOW = pressed          -> stop

    // Normally CLOSED (recommended): a closed loop to GND holds the pin LOW to
    // AUTHORISE running.
    CHECK(!estopAssertedFor(false, true));   // LOW = healthy loop     -> run
    CHECK(estopAssertedFor(true, true));     // HIGH = pressed OR CUT  -> stop
}

// The point of the normally-closed wiring: a broken chain must fail SAFE. A cut
// wire, an unplugged connector and a corroded contact all leave the pull-up to
// pull the pin HIGH, and that must read as "stop" — not as "everything is fine",
// which is what the normally-open reading of the same level would say.
TEST(estop_normally_closed_fails_safe_on_a_broken_chain) {
    const bool chainBroken = true;   // pull-up wins: the pin floats HIGH
    CHECK(estopAssertedFor(chainBroken, /*normallyClosed=*/true));
    // The same level on a normally-open button means "released", which is exactly
    // why the two wirings must never share a hard-coded level test.
    CHECK(!estopAssertedFor(chainBroken, /*normallyClosed=*/false));
}

// The regression itself: a hard-coded `level == LOW` check agrees with
// estopAssertedFor() only for the normally-open wiring, and is INVERTED for the
// normally-closed one — in both directions.
TEST(estop_a_hardcoded_low_test_is_inverted_on_a_closed_loop) {
    auto hardcodedLowMeansStop = [](bool pinHigh) { return !pinHigh; };
    for (bool pinHigh : {false, true}) {
        CHECK_EQ((int)hardcodedLowMeansStop(pinHigh),
                 (int)estopAssertedFor(pinHigh, /*normallyClosed=*/false));
        CHECK(hardcodedLowMeansStop(pinHigh) !=
              estopAssertedFor(pinHigh, /*normallyClosed=*/true));
    }
}
