// Shared rig for the two runtime harnesses (SafetySupervisor, PlaybackScheduler).
//
// Both components are Arduino-gated, so until now they were only COMPILE-checked —
// and between them they own arming, homing, parking, the runtime fault path, and
// every mechanical decision a note goes through. "It builds" is a weak claim to
// make about that.
//
// These harnesses build the REAL SafetySupervisor.h / PlaybackScheduler.h against
// the REAL StepperBank / ServoBank, on top of the instrumented FastAccelStepper and
// PCA9685 stubs the existing bank harnesses already use, plus a controllable
// digitalRead()/millis() so a homing sequence — "watch a sensor change while time
// passes" — is actually observable.
//
// What this can and cannot prove: it proves the SEQUENCING and the decisions
// (what gets cut, in what order, on which fault, and what refuses to arm). It
// proves nothing about timing on real silicon, I2C behaviour under load, or
// whether a finger physically clears a string. Those stay bench work.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "../../src/core/configuration/Profile.h"
#include "../../src/platform/esp32/PlaybackScheduler.h"
#include "../../src/platform/esp32/SafetySupervisor.h"

namespace rig {

inline int g_fail = 0;
inline const char* g_case = "";

inline void beginCase(const char* name) { g_case = name; }

inline void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("  [FAIL] %s — %s\n", g_case, what);
        ++g_fail;
    }
}

inline int finish(const char* harness) {
    if (g_fail == 0) std::printf("\n%s OK\n", harness);
    else std::printf("\n%s: %d failure(s)\n", harness, g_fail);
    return g_fail == 0 ? 0 : 1;
}

// A minimal but REAL profile: two axes, a finger + pluck servo each on direct
// GPIO (no PCA, so the I2C stub stays out of the way unless a test wants it).
inline gmb::Profile twoStringProfile() {
    gmb::Profile p = gmb::Profile::makeDefault("Rig", 2, {67, 60}, 12);
    for (auto& a : p.strings) {
        a.enabled = true;
        a.transmission = gmb::Transmission::Custom;
        a.customStepsPerMm = 100.0;
        a.minPositionMm = 0.0;
        a.maxPositionMm = 200.0;
        a.maxSpeedMmS = 200.0;
        a.maxAccelMmS2 = 1000.0;
    }
    // Homing: seek toward home, sensor active LOW (the common NC/NPN switch).
    for (auto& h : p.homing) {
        h.direction = -1;
        h.sensorActiveHigh = false;
        h.limitActiveHigh = false;
        h.fastSpeedMmS = 50.0;
        h.slowSpeedMmS = 5.0;
        h.backoffMm = 1.0;
        h.offsetMm = 0.0;
        h.timeoutMs = 3000;
        h.maxSearchMm = 400.0;
    }
    p.servos.clear();
    for (int i = 0; i < 2; ++i) {
        gmb::ServoConfig f;
        f.enabled = true;
        f.function = "finger";
        f.stringIndex = static_cast<int8_t>(i);
        f.source = gmb::ServoSource::DirectGpio;
        f.gpio = static_cast<int8_t>(10 + i);
        f.travelMs = 40;
        f.settleMs = 10;
        p.servos.push_back(f);
        gmb::ServoConfig k;
        k.enabled = true;
        k.function = "pluck";
        k.stringIndex = static_cast<int8_t>(i);
        k.source = gmb::ServoSource::DirectGpio;
        k.gpio = static_cast<int8_t>(14 + i);   // 19/20 are USB-reserved on the S3
        k.travelMs = 30;
        k.settleMs = 5;
        p.servos.push_back(k);
    }
    return p;
}

// HOME pins for the two axes, in the pin-level table.
constexpr int kHomePin0 = 4;
constexpr int kHomePin1 = 5;
constexpr int kLimitPin0 = 6;
constexpr int kLimitPin1 = 7;
constexpr int kEstopPin = 8;

inline std::vector<gmb::AxisPins> rigAxisPins() {
    std::vector<gmb::AxisPins> v;
    gmb::AxisPins a0; a0.step = 1; a0.dir = 2; a0.home = kHomePin0; a0.limit = kLimitPin0;
    gmb::AxisPins a1; a1.step = 12; a1.dir = 13; a1.home = kHomePin1; a1.limit = kLimitPin1;
    v.push_back(a0);
    v.push_back(a1);
    return v;
}

// Advance the whole rig by one tick: the kinematic stepper model, then the HOME
// sensors derived from where the carriages actually are.
//
// The sensor is modelled as sitting AT the origin: it asserts while the carriage is
// within a switch's worth of 0 mm. That is what makes a homing sequence converge —
// approach, assert, back off until it RELEASES, approach slowly, assert again — and
// none of it is observable with a sensor pinned to one level.
inline void tickSensors(gmb::StepperBank& steppers, uint32_t dtMs) {
    gmbAdvanceSteppers(dtMs);
    constexpr double kSwitchWidthMm = 2.0;
    const int homePins[2] = {kHomePin0, kHomePin1};
    for (size_t i = 0; i < 2 && i < steppers.count(); ++i) {
        bool atHome = steppers.positionMm(i) <= kSwitchWidthMm;
        gmbSetPinLevel(homePins[i], atHome ? LOW : HIGH);  // sensorActiveHigh=false
    }
}

// Sensors idle HIGH (not asserted, since activeHigh=false), E-stop released on an
// NO contact (HIGH = healthy with the pull-up).
inline void resetPins() {
    gmbSetPinLevel(kHomePin0, HIGH);
    gmbSetPinLevel(kHomePin1, HIGH);
    gmbSetPinLevel(kLimitPin0, HIGH);
    gmbSetPinLevel(kLimitPin1, HIGH);
    gmbSetPinLevel(kEstopPin, HIGH);
    gmbSetMillis(0);
}

}  // namespace rig
