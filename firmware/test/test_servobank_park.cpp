// Audit 7: the controlled park REPORTS failures (ParkResult) instead of being
// fire-and-forget — arming / a profile swap must abort when a rest command never
// reached a servo, because "the fingers lifted" can then not be assumed. Uses the
// host-only write-fault hook (the native build has no hardware able to refuse a
// write, so failure paths need injection).
#include "TestFramework.h"

#include "../src/platform/esp32/ServoBank.h"

using namespace gmb;

namespace {

ServoConfig parkServo(const char* fn, int str) {
    ServoConfig s;
    s.enabled = true;
    s.function = fn;
    s.stringIndex = static_cast<int8_t>(str);
    s.source = ServoSource::Pca;
    s.pcaBoard = 0;
    s.channel = static_cast<uint8_t>(str);
    s.pulseMinUs = 500;
    s.pulseMaxUs = 2500;
    s.restUs = 1500;
    s.activeUs = 1900;
    s.travelMs = 80;
    s.settleMs = 20;
    return s;
}

}  // namespace

TEST(servobank_park_ok_and_disabled_skipped) {
    ServoBank b;
    std::vector<ServoConfig> sv = {parkServo("finger", 0), parkServo("pluck", 0),
                                   parkServo("finger", 1)};
    sv[1].enabled = false;  // disabled: never driven, so never a park failure
    b.begin(sv, -1, -1, -1);
    b.outputEnable(true);
    ServoBank::ParkResult r = b.moveAllToRest();
    CHECK(r.ok);
    CHECK_EQ(r.failedServo, -1);
    CHECK(r.reason == ActuatorResult::Ok);
    CHECK_EQ((int)b.lastCommandedUs(0), 1500);  // enabled servos commanded to rest
    CHECK_EQ((int)b.lastCommandedUs(2), 1500);
    CHECK_EQ((int)b.lastCommandedUs(1), 0);     // the disabled one was left alone
}

TEST(servobank_park_reports_first_failure) {
    ServoBank b;
    std::vector<ServoConfig> sv = {parkServo("finger", 0), parkServo("finger", 1),
                                   parkServo("pluck", 1)};
    b.begin(sv, -1, -1, -1);
    b.outputEnable(true);
    // Servos 1 AND 2 refuse their write: the report must carry the FIRST one.
    b.hostWriteResult = [](int idx) {
        return idx >= 1 ? ActuatorResult::OutputFault : ActuatorResult::Ok;
    };
    ServoBank::ParkResult r = b.moveAllToRest();
    CHECK(!r.ok);
    CHECK_EQ(r.failedServo, 1);
    CHECK(r.reason == ActuatorResult::OutputFault);

    // With the fault gone the same bank parks cleanly again.
    b.hostWriteResult = nullptr;
    ServoBank::ParkResult r2 = b.moveAllToRest();
    CHECK(r2.ok);
}

// ---- governed park (audit P0) ----------------------------------------------
// Arming / a profile swap must never start every servo at once: the rest sweep
// goes through a dedicated governor with the profile's power caps, so the
// electrical worst case of a park equals normal play, not "all servos together".

TEST(governed_park_spreads_starts_with_the_power_caps) {
    ServoBank b;
    // Four servos on the same PCA board; travel 20 + settle 5.
    std::vector<ServoConfig> sv = {parkServo("finger", 0), parkServo("finger", 1),
                                   parkServo("finger", 2), parkServo("pluck", 0)};
    for (auto& s : sv) { s.travelMs = 20; s.settleMs = 5; }
    b.begin(sv, -1, -1, -1);
    b.outputEnable(true);

    // Global cap 1, stagger 10 ms: starts land at t0, t0+10, t0+20, t0+30, and the
    // estimate is that schedule plus the slowest travel+settle (30 + 25 = 55).
    uint32_t t0 = 1000;
    uint32_t est = b.beginGovernedPark(t0, /*global=*/1, /*perBoard=*/0, /*stagger=*/10);
    CHECK_EQ((int)est, 55);

    b.serviceGovernedPark(t0);
    CHECK_EQ((int)b.lastCommandedUs(0), 1500);  // only the first slot fired
    CHECK_EQ((int)b.lastCommandedUs(1), 0);
    b.serviceGovernedPark(t0 + 5);              // window still full
    CHECK_EQ((int)b.lastCommandedUs(1), 0);
    b.serviceGovernedPark(t0 + 10);
    CHECK_EQ((int)b.lastCommandedUs(1), 1500);
    CHECK_EQ((int)b.lastCommandedUs(2), 0);
    b.serviceGovernedPark(t0 + 20);
    b.serviceGovernedPark(t0 + 30);
    CHECK_EQ((int)b.lastCommandedUs(3), 1500);  // last start at t0+30
    CHECK(!b.governedParkDone(t0 + 30));        // still travelling
    CHECK(!b.governedParkDone(t0 + 54));
    CHECK(b.governedParkDone(t0 + 55));         // matches the estimate exactly
}

TEST(governed_park_per_board_cap_exempts_direct_servos) {
    ServoBank b;
    // Two direct-GPIO servos: the per-board cap must NOT serialise them — a servo
    // on no PCA board (bucket 0xFF) only answers to the global cap, exactly like
    // the play-time governor.
    ServoConfig d0 = parkServo("finger", 0), d1 = parkServo("pluck", 0);
    for (ServoConfig* s : {&d0, &d1}) {
        s->source = ServoSource::DirectGpio;
        s->gpio = (s == &d0) ? 4 : 5;
        s->travelMs = 20; s->settleMs = 5;
    }
    std::vector<ServoConfig> sv = {d0, d1};
    b.begin(sv, -1, -1, -1);
    b.outputEnable(true);
    uint32_t t0 = 500;
    b.beginGovernedPark(t0, /*global=*/0, /*perBoard=*/1, /*stagger=*/10);
    b.serviceGovernedPark(t0);
    CHECK_EQ((int)b.lastCommandedUs(0), 1500);  // both start the same tick
    CHECK_EQ((int)b.lastCommandedUs(1), 1500);
    CHECK(b.governedParkDone(t0 + 25));
}

TEST(governed_park_with_stagger_zero_matches_ungoverned_park) {
    ServoBank b;
    std::vector<ServoConfig> sv = {parkServo("finger", 0), parkServo("finger", 1)};
    b.begin(sv, -1, -1, -1);
    b.outputEnable(true);
    uint32_t t0 = 100;
    // staggerMs == 0 turns the governor off: everything starts at once and the
    // estimate degenerates to parkDurationMs() — the historical behaviour.
    uint32_t est = b.beginGovernedPark(t0, 3, 2, /*stagger=*/0);
    CHECK_EQ((int)est, (int)b.parkDurationMs());
    b.serviceGovernedPark(t0);
    CHECK_EQ((int)b.lastCommandedUs(0), 1500);
    CHECK_EQ((int)b.lastCommandedUs(1), 1500);
    CHECK(b.governedParkDone(t0 + b.parkDurationMs()));
}

TEST(governed_park_reports_refused_write_from_a_later_slot) {
    ServoBank b;
    std::vector<ServoConfig> sv = {parkServo("finger", 0), parkServo("finger", 1)};
    b.begin(sv, -1, -1, -1);
    b.outputEnable(true);
    b.hostWriteResult = [](int idx) {
        return idx == 1 ? ActuatorResult::OutputFault : ActuatorResult::Ok;
    };
    uint32_t t0 = 100;
    b.beginGovernedPark(t0, 1, 0, 10);
    ServoBank::ParkResult r = b.serviceGovernedPark(t0);
    CHECK(r.ok);                                  // slot 0 fine
    r = b.serviceGovernedPark(t0 + 10);           // slot 1 refuses its write
    CHECK(!r.ok);
    CHECK_EQ(r.failedServo, 1);
    CHECK(r.reason == ActuatorResult::OutputFault);
    CHECK(!b.governedParkResult().ok);            // sticky for the swap-time check
    b.hostWriteResult = nullptr;
}

TEST(hard_stop_cancels_a_governed_park) {
    ServoBank b;
    std::vector<ServoConfig> sv = {parkServo("finger", 0), parkServo("finger", 1)};
    b.begin(sv, -1, -1, -1);
    b.outputEnable(true);
    uint32_t t0 = 100;
    b.beginGovernedPark(t0, 1, 0, 10);
    b.serviceGovernedPark(t0);
    b.hardStop();
    // The park is gone with the stop: no completion, and no further slot fires.
    CHECK(!b.governedParkDone(t0 + 10000));
    b.serviceGovernedPark(t0 + 10);
    CHECK_EQ((int)b.lastCommandedUs(1), 0);  // rt_ cleared, nothing new commanded
}
