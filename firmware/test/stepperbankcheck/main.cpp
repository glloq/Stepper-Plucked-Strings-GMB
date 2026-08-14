// Runtime check for the ARDUINO branch of StepperBank — the motion-side twin of
// servobankcheck (audit P0.3 / P1.4).
//
// The native unit tests only reach the pure core (StepperAxis, MotionPlanner,
// HomingController); the branch that actually talks to the step engine is
// Arduino-gated, so before this harness it was only ever COMPILE-checked. Here the
// REAL src/platform/esp32/StepperBank.cpp is built against an instrumented
// FastAccelStepper stub that records every engine call, which lets the test prove:
//
//   * moveToMm reports Ok / Disabled / OutputFault instead of silently doing
//     nothing (P1.4) — a refused carriage move must fault the axis;
//   * hardStop() FORCE-stops every axis and drops ENABLE, with no deceleration and
//     no dependency on a move finishing (P0.3, spec §21.2);
//   * controlledStopAll() asks for a DECELERATED stop and leaves the drivers
//     enabled — the profile-change path, which must never lose steps;
//   * an axis that could not attach a hardware step generator is faulted per-axis
//     and refuses commands, rather than pretending to move.
//
// Pure g++, no xtensa toolchain.
#include <cstdio>
#include <string>
#include <vector>

#include "../../src/platform/esp32/StepperBank.h"

using namespace gmb;

static int g_fail = 0;
#define CHECK(cond, msg)                                           \
  do {                                                            \
    if (!(cond)) { std::printf("  [FAIL] %s\n", msg); ++g_fail; } \
  } while (0)

static int countLog(const char* what) {
  int n = 0;
  for (const auto& e : g_stepLog)
    if (e.what == what) ++n;
  return n;
}

static AxisConfig axis(bool enabled = true) {
  AxisConfig a;
  a.enabled = enabled;
  a.transmission = Transmission::Custom;
  a.customStepsPerMm = 100.0;   // 1 mm = 100 steps: easy exact assertions
  a.minPositionMm = 0.0;
  a.maxPositionMm = 200.0;
  a.maxSpeedMmS = 200.0;
  a.maxAccelMmS2 = 1000.0;      // stop time = v/a = 0.2 s -> 250 ms with the floor
  return a;
}

static AxisPins pins(int8_t step, int8_t dir, int8_t home, int8_t limit) {
  AxisPins p;
  p.step = step; p.dir = dir; p.home = home; p.limit = limit;
  return p;
}

int main() {
  // ---- P1.4: every motion command reports whether it reached the engine -----
  {
    g_maxGenerators = 8;
    std::vector<AxisConfig> axes = {axis(true), axis(false), axis(true)};
    std::vector<AxisPins> ap = {pins(4, 5, 12, 13), pins(-1, -1, -1, -1),
                                pins(6, 7, 14, 15)};
    StepperBank bank;
    bank.begin(axes, ap, /*enablePin=*/42);

    CHECK(!bank.attachFault(), "every enabled axis attached a step generator");
    CHECK(bank.moveToMm(0, 50.0) == ActuatorResult::Ok, "moveToMm(enabled) -> Ok");
    // A disabled axis is a configuration statement, not a failure: it reports
    // Disabled so the caller skips it without faulting anything.
    CHECK(bank.moveToMm(1, 50.0) == ActuatorResult::Disabled,
          "moveToMm(disabled axis) -> Disabled");
    CHECK(bank.moveToMm(9, 50.0) == ActuatorResult::InvalidIndex,
          "moveToMm(out of range) -> InvalidIndex");
    CHECK(bank.setVelocityMm(0, 30.0) == ActuatorResult::Ok, "setVelocityMm -> Ok");
    CHECK(bank.moveToMmRaw(0, -20.0) == ActuatorResult::Ok,
          "moveToMmRaw (unclamped, homing) -> Ok");

    // The soft limits really clamp: 500 mm on a 0..200 axis lands at 200 mm.
    g_stepLog.clear();
    CHECK(bank.moveToMm(0, 500.0) == ActuatorResult::Ok, "over-travel move accepted");
    bool clamped = false;
    for (const auto& e : g_stepLog)
      if (e.what == "moveTo" && e.value == 20000) clamped = true;  // 200 mm * 100
    CHECK(clamped, "moveToMm clamps to maxPositionMm (200 mm -> 20000 steps)");

    // moveCount() only counts POSITION moves — a homing velocity cruise is not a
    // musical carriage move and must not inflate the diagnostics counter.
    uint32_t before = bank.moveCount();
    bank.setVelocityMm(0, 10.0);
    CHECK(bank.moveCount() == before, "setVelocityMm does not count as a move");
    bank.moveToMm(0, 10.0);
    CHECK(bank.moveCount() == before + 1, "moveToMm bumps moveCount");
  }

  // ---- P0.3: hardStop is immediate, controlledStopAll decelerates -----------
  {
    g_maxGenerators = 8;
    std::vector<AxisConfig> axes = {axis(true), axis(true)};
    std::vector<AxisPins> ap = {pins(4, 5, 12, 13), pins(6, 7, 14, 15)};
    StepperBank bank;
    bank.begin(axes, ap, /*enablePin=*/42);
    bank.enableDrivers(true);
    CHECK(bank.enabled(), "drivers enabled");

    g_stepLog.clear();
    bank.hardStop();
    CHECK(countLog("forceStop") == 2, "hardStop force-stops BOTH axes");
    CHECK(countLog("stopMove") == 0, "hardStop never uses a decelerated stop");
    CHECK(!bank.enabled(), "hardStop drops the driver ENABLE");

    bank.enableDrivers(true);
    g_stepLog.clear();
    bank.controlledStopAll();
    CHECK(countLog("stopMove") == 2, "controlledStopAll decelerates BOTH axes");
    CHECK(countLog("forceStop") == 0, "controlledStopAll never force-stops");
    CHECK(bank.enabled(),
          "controlledStopAll keeps the drivers enabled (the caller cuts power after "
          "stopDurationMs)");

    // stopDurationMs is the wait a controlled park must allow: v/a plus a floor for
    // the engine's own reaction latency. 200 / 1000 = 0.2 s -> 200 + 50 = 250 ms.
    CHECK(bank.stopDurationMs() == 250, "stopDurationMs = v/a + 50 ms floor");
  }

  // ---- an axis with no step generator is faulted, and refuses commands ------
  {
    g_maxGenerators = 1;  // only ONE unit free: the second enabled axis cannot attach
    std::vector<AxisConfig> axes = {axis(true), axis(true)};
    std::vector<AxisPins> ap = {pins(4, 5, 12, 13), pins(6, 7, 14, 15)};
    StepperBank bank;
    bank.begin(axes, ap, /*enablePin=*/42);

    CHECK(bank.attachFault(), "bank reports an attach fault");
    CHECK(!bank.axisAttachFault(0), "axis 0 attached");
    CHECK(bank.axisAttachFault(1), "axis 1 could not attach a step generator");
    CHECK(bank.moveToMm(0, 10.0) == ActuatorResult::Ok, "the attached axis still moves");
    // The whole point of P1.4: a command to an axis with no generator must REPORT
    // the failure so the runtime faults it, not return void and be assumed done.
    CHECK(bank.moveToMm(1, 10.0) == ActuatorResult::OutputFault,
          "moveToMm on an unattached axis -> OutputFault");
    CHECK(bank.setVelocityMm(1, 10.0) == ActuatorResult::OutputFault,
          "setVelocityMm on an unattached axis -> OutputFault");
    g_maxGenerators = 8;  // restore for any later case
  }

  // ---- a disabled axis never attaches and never faults the bank ------------
  {
    g_maxGenerators = 8;
    std::vector<AxisConfig> axes = {axis(false)};
    std::vector<AxisPins> ap = {pins(-1, -1, -1, -1)};  // legitimately no pins
    StepperBank bank;
    bank.begin(axes, ap, /*enablePin=*/42);
    CHECK(!bank.attachFault(),
          "a disabled axis with no pins does not fault the bank");
    CHECK(bank.stopDurationMs() == 0, "no enabled axis -> nothing to wait for");
  }

  std::printf(g_fail ? "\nSTEPPERBANKCHECK FAILED (%d)\n" : "\nstepperbankcheck OK\n",
              g_fail);
  return g_fail ? 1 : 0;
}
