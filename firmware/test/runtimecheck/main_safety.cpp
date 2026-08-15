// Behavioural check for SafetySupervisor — the arming / homing / hard-stop / panic
// / runtime-fault component. Until now it was compile-checked only, and its own
// header said so.
//
// Each case drives the REAL component against the real StepperBank / ServoBank on
// instrumented stubs, and asserts what the machine actually did: what got cut, in
// what order, and what refused to arm.
#include "common.h"

using namespace gmb;
using namespace rig;

namespace {

// One assembled machine, wired the way main.cpp wires it.
struct Rig {
    Profile profile = twoStringProfile();
    SafetyManager safety;
    StepperBank steppers;
    ServoBank servos;
    InstrumentController instrument;
    PlaybackScheduler scheduler;
    SafetySupervisor supervisor;
    Diagnostics diag;
    AppPhase phase = AppPhase::Boot;
    bool degraded = false;
    bool homingStarted = false;
    std::vector<HomingController> homing;
    std::vector<bool> anchored;
    std::vector<bool> axisFaulted;
    int8_t estopPin = kEstopPin;
    int rebuildCalls = 0;
    int notifyCalls = 0;
    int cleanupCalls = 0;
    int readyCalls = 0;
    int workingAxes = 2;   // what rebuildCaps reports

    void build(bool withEstop = true) {
        resetPins();
        safety.boot();
        instrument.load(profile);
        std::vector<AxisPins> pins = rigAxisPins();
        // Endstops as this rig wires them: active-low contacts, so the struct's
        // own defaults describe both axes and the call needs no per-axis table.
        steppers.begin(profile.strings, pins, /*enablePin=*/3,
                       {AxisEndstops{}, AxisEndstops{}});
        servos.begin(profile.servos, -1, -1, -1, -1, -1);
        homing.assign(profile.strings.size(), HomingController{});
        for (size_t i = 0; i < homing.size(); ++i) homing[i].configure(profile.homing[i]);
        anchored.assign(profile.strings.size(), false);
        axisFaulted.assign(profile.strings.size(), false);
        scheduler.configure(profile.strings.size());
        estopPin = withEstop ? kEstopPin : int8_t(-1);

        PlaybackScheduler::Deps sd;
        sd.instrument = &instrument;
        sd.steppers = &steppers;
        sd.servos = &servos;
        sd.actuators = &actuators;
        sd.diag = &diag;
        sd.profile = &profile;
        sd.axisFaulted = &axisFaulted;
        sd.fault = [this](size_t i, const char* r, uint32_t t) {
            supervisor.faultRuntimeAxis(i, r, t);
        };
        sd.actOk = [](size_t, ActuatorResult r, const char*, uint32_t) { return ok(r); };
        scheduler.bind(sd);

        SafetySupervisor::Deps yd;
        yd.safety = &safety;
        yd.steppers = &steppers;
        yd.servos = &servos;
        yd.instrument = &instrument;
        yd.scheduler = &scheduler;
        yd.diag = &diag;
        yd.profile = &profile;
        yd.phase = &phase;
        yd.degraded = &degraded;
        yd.homingStarted = &homingStarted;
        yd.homing = &homing;
        yd.anchored = &anchored;
        yd.axisFaulted = &axisFaulted;
        yd.estopPin = &estopPin;
        yd.rebuildCaps = [this]() { ++rebuildCalls; return workingAxes; };
        yd.notifyCaps = [this]() { ++notifyCalls; };
        yd.hardStopCleanup = [this]() { ++cleanupCalls; };
        yd.onReady = [this]() { ++readyCalls; };
        supervisor.bind(yd);
    }

    ActuatorManager actuators;

    // Run homing to completion. The carriages start away from home and seek toward
    // it; tickSensors() moves them and derives the HOME level from where they are.
    void runHoming(uint32_t untilMs = 8000) {
        for (uint32_t t = 0; t <= untilMs; t += 5) {
            gmbSetMillis(t);
            tickSensors(steppers, 5);
            steppers.updateSensors(t);
            servos.update(t);
            if (phase == AppPhase::Homing) supervisor.tickHoming(t);
            if (phase != AppPhase::Homing) break;
        }
    }

    // Put the carriages away from the switch so a seek has somewhere to come from.
    void placeAwayFromHome() {
        gmbSetPinLevel(kHomePin0, HIGH);
        gmbSetPinLevel(kHomePin1, HIGH);
    }
};

// ---- arming refuses when it must ------------------------------------------

void estop_asserted_refuses_to_home() {
    beginCase("E-stop asserted refuses to arm");
    Rig r;
    r.build();
    // NO contact: LOW = pressed.
    gmbSetPinLevel(kEstopPin, LOW);
    check(!r.supervisor.beginHoming(0), "beginHoming must refuse while E-stop is asserted");
    check(r.safety.state() == SafetyState::EmergencyStop,
          "an asserted E-stop at pre-arm must latch EmergencyStop, not be ignored");
    check(r.phase != AppPhase::Homing, "phase must not advance to Homing");
}

// The bug this whole predicate exists for: on the RECOMMENDED normally-closed
// chain, a healthy loop holds the pin LOW. Read as "LOW means pressed" the machine
// could never home; read correctly it homes, and an OPEN loop (cut wire) stops it.
void normally_closed_polarity_is_the_right_way_round() {
    beginCase("normally-closed E-stop polarity");
    Rig r;
    r.profile.estopNormallyClosed = true;
    r.build();
    gmbSetPinLevel(kEstopPin, LOW);    // closed loop = healthy
    check(!r.supervisor.estopAssertedNow(), "a CLOSED NC loop must read as healthy");
    check(r.supervisor.beginHoming(0), "a healthy NC chain must allow homing");

    Rig r2;
    r2.profile.estopNormallyClosed = true;
    r2.build();
    gmbSetPinLevel(kEstopPin, HIGH);   // loop open: pressed, cut wire, unplugged
    check(r2.supervisor.estopAssertedNow(), "an OPEN NC loop must read as asserted");
    check(!r2.supervisor.beginHoming(0), "an open NC chain must refuse to home");
}

void latched_panic_refuses_to_home() {
    beginCase("latched panic refuses to arm");
    Rig r;
    r.build();
    r.supervisor.panic();
    check(r.supervisor.locked(), "panic must latch");
    check(!r.supervisor.beginHoming(0), "a latched panic must refuse homing");
    check(!r.supervisor.reset(0) || r.phase == AppPhase::Homing,
          "reset must either refuse or actually start a re-home");
}

// ---- homing ----------------------------------------------------------------

void homing_reaches_ready_and_anchors_every_axis() {
    beginCase("homing reaches Ready");
    Rig r;
    r.build();
    check(r.supervisor.beginHoming(0), "homing must start on a valid profile");
    check(r.phase == AppPhase::Homing, "phase must be Homing");
    r.runHoming();
    check(r.phase == AppPhase::Ready, "homing must reach Ready");
    check(r.safety.actuatorsAllowed(), "Ready must mean actuators are allowed");
    check(r.anchored[0] && r.anchored[1], "every axis must be anchored");
    check(r.readyCalls == 1, "onReady must fire exactly once");
    check(!r.degraded, "a clean homing must not report degraded");
}

// A LIMIT during the seek means the HOME sensor was missed. That axis must drop
// out; the machine must not grind on to its timeout.
void limit_during_homing_faults_only_that_axis() {
    beginCase("LIMIT during homing");
    Rig r;
    r.build();
    check(r.supervisor.beginHoming(0), "homing must start");
    // Axis 0 hits its LIMIT; axis 1 homes normally.
    for (uint32_t t = 0; t <= 8000; t += 5) {
        gmbSetMillis(t);
        tickSensors(r.steppers, 5);
        if (t >= 100) gmbSetPinLevel(kLimitPin0, LOW);   // limitActiveHigh=false
        r.steppers.updateSensors(t);
        r.servos.update(t);
        if (r.phase == AppPhase::Homing) r.supervisor.tickHoming(t);
        if (r.phase != AppPhase::Homing) break;
    }
    check(r.axisFaulted[0], "the axis that hit LIMIT must be faulted");
    check(!r.axisFaulted[1], "the healthy axis must be untouched");
    check(r.phase == AppPhase::Ready, "the machine must still arm on the good axis");
    check(r.rebuildCalls > 0, "capabilities must be rebuilt for a degraded run");
    check(r.notifyCalls > 0, "the change must be announced");
}

// With no axis able to home there is nothing to play: arming would advertise a
// bogus 0..0 range. It must stay out of Ready.
void no_axis_homed_refuses_to_arm() {
    beginCase("zero working axes");
    Rig r;
    r.build();
    check(r.supervisor.beginHoming(0), "homing must start");
    // Neither HOME ever asserts (the carriages are held away from the switch):
    // both controllers must time out rather than seek forever.
    for (uint32_t t = 0; t <= 20000; t += 25) {
        gmbSetMillis(t);
        gmbAdvanceSteppers(25);
        gmbSetPinLevel(kHomePin0, HIGH);
        gmbSetPinLevel(kHomePin1, HIGH);
        r.steppers.updateSensors(t);
        r.servos.update(t);
        if (r.phase == AppPhase::Homing) r.supervisor.tickHoming(t);
        if (r.phase != AppPhase::Homing) break;
    }
    check(r.phase == AppPhase::Boot, "with no axis homed the machine must fall back to Boot");
    check(!r.safety.actuatorsAllowed(), "actuators must NOT be allowed");
}

// ---- runtime faults --------------------------------------------------------

void last_axis_fault_hard_stops_and_panics() {
    beginCase("last operational axis fails");
    Rig r;
    r.build();
    r.supervisor.beginHoming(0);
    r.runHoming();
    check(r.phase == AppPhase::Ready, "precondition: Ready");
    r.workingAxes = 1;
    r.supervisor.faultRuntimeAxis(0, "test fault", 5000);
    check(r.axisFaulted[0], "axis 0 must be faulted");
    check(r.safety.actuatorsAllowed(), "one working axis left: still armed (degraded)");
    r.workingAxes = 0;   // now the last one goes
    r.supervisor.faultRuntimeAxis(1, "test fault", 6000);
    check(!r.safety.actuatorsAllowed(), "with zero working axes the machine must not stay armed");
    check(r.safety.state() == SafetyState::Panic, "it must latch a panic");
    check(r.phase == AppPhase::Boot, "and drop out of Ready");
}

// A PCA9685 that stops responding takes out only the strings it drives. With no
// string mapped to it the loss is not attributable, so it must fail safe.
void unattributable_pca_loss_panics() {
    beginCase("PCA lost with no string mapped");
    Rig r;
    r.build();   // this rig uses direct-GPIO servos: no string maps to a PCA board
    r.supervisor.beginHoming(0);
    r.runHoming();
    check(r.phase == AppPhase::Ready, "precondition: Ready");
    r.supervisor.serviceLostPcaBoard(/*board=*/0, 7000);
    check(r.safety.state() == SafetyState::Panic,
          "an unattributable PCA loss must fail safe, not be ignored");
    check(!r.axisFaulted[0] && !r.axisFaulted[1],
          "no individual axis should be blamed for a shared-resource loss");
}

// ---- hard stop -------------------------------------------------------------

void hard_stop_cuts_everything_and_leaves_ready() {
    beginCase("hardStopAll");
    Rig r;
    r.build();
    r.supervisor.beginHoming(0);
    r.runHoming();
    check(r.phase == AppPhase::Ready, "precondition: Ready");
    r.supervisor.hardStopAll();
    check(r.phase == AppPhase::Boot, "hard stop must leave Ready");
    check(r.cleanupCalls == 1, "the app-level cleanup must run exactly once");
    check(r.scheduler.idle(0) && r.scheduler.idle(1),
          "every string's FSM must be cleared, not left mid-sequence");
}

// A hardware E-stop outranks a software panic: panic() must never downgrade a
// latched EmergencyStop.
void panic_never_downgrades_an_emergency_stop() {
    beginCase("panic does not downgrade E-stop");
    Rig r;
    r.build();
    r.supervisor.emergencyStop();
    check(r.safety.state() == SafetyState::EmergencyStop, "precondition: E-stop latched");
    r.supervisor.panic();
    check(r.safety.state() == SafetyState::EmergencyStop,
          "a later software panic must not replace the E-stop state");
}

// Recovery is explicit and conditional: it must refuse while the E-stop is still
// held, and clear the runtime axis faults when it does proceed.
void reset_refuses_while_estop_held_then_recovers() {
    beginCase("reset preconditions");
    Rig r;
    r.build();
    r.supervisor.beginHoming(0);
    r.runHoming();
    r.workingAxes = 1;
    r.supervisor.faultRuntimeAxis(0, "test fault", 5000);
    check(r.axisFaulted[0], "precondition: an axis is faulted");

    gmbSetPinLevel(kEstopPin, LOW);      // still pressed (NO contact)
    check(!r.supervisor.reset(6000), "reset must refuse while the E-stop is asserted");
    check(r.axisFaulted[0], "a refused reset must not clear the fault");

    gmbSetPinLevel(kEstopPin, HIGH);     // released
    r.workingAxes = 2;
    check(r.supervisor.reset(7000), "reset must proceed once the E-stop is released");
    check(!r.axisFaulted[0], "reset must recover runtime-faulted axes");
    check(r.phase == AppPhase::Homing, "recovery must force a re-home before playing");
}

}  // namespace

int main() {
    std::printf("SafetySupervisor runtime check\n");
    estop_asserted_refuses_to_home();
    normally_closed_polarity_is_the_right_way_round();
    latched_panic_refuses_to_home();
    homing_reaches_ready_and_anchors_every_axis();
    limit_during_homing_faults_only_that_axis();
    no_axis_homed_refuses_to_arm();
    last_axis_fault_hard_stops_and_panics();
    unattributable_pca_loss_panics();
    hard_stop_cuts_everything_and_leaves_ready();
    panic_never_downgrades_an_emergency_stop();
    reset_refuses_while_estop_held_then_recovers();
    return finish("safetysupervisorcheck");
}
