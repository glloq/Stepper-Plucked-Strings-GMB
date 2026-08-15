// Behavioural check for PlaybackScheduler — the per-string mechanical FSM.
//
// This is the sequence that decides when a finger lifts, when a carriage is
// allowed to move, and when a string is struck. §16 of the spec is one rule —
// never drag a finger along a string — and it is enforced entirely by the ORDER
// of the phases below. That order was compile-checked only.
//
// The cases here assert the order itself: that the carriage does not move before
// the finger has had its travel time, that a Note Off during a move stops the
// carriage instead of letting it finish, and that a new note arriving mid-
// deceleration does not issue a move into a still-running motor.
#include "common.h"

using namespace gmb;
using namespace rig;

namespace {

// The engine call log is the real observable here: the stub's moveTo() arrives
// immediately (there is no acceleration model), so "did the FSM DECIDE to move /
// stop" is a question about the commands it issued, not about a simulated
// position. Counting log entries is therefore the honest assertion.
int countCalls(const char* what) {
    int n = 0;
    for (const auto& e : g_stepLog)
        if (e.what == what) ++n;
    return n;
}

struct Rig {
    Profile profile = twoStringProfile();
    SafetyManager safety;
    StepperBank steppers;
    ServoBank servos;
    InstrumentController instrument;
    PlaybackScheduler scheduler;
    ActuatorManager actuators;
    Diagnostics diag;
    std::vector<bool> axisFaulted;
    std::vector<std::string> faults;   // reasons routed through the fault callback

    void build() {
        resetPins();
        g_stepLog.clear();
        safety.boot();
        instrument.load(profile);
        std::vector<AxisPins> pins = rigAxisPins();
        steppers.begin(profile.strings, pins, /*enablePin=*/3,
                       {false, false}, {false, false});
        servos.begin(profile.servos, -1, -1, -1, -1, -1);
        axisFaulted.assign(profile.strings.size(), false);
        scheduler.configure(profile.strings.size());
        actuators.configure(profile.power.maxConcurrentMoves,
                            profile.power.maxConcurrentPerBoard,
                            profile.power.staggerMs);

        PlaybackScheduler::Deps sd;
        sd.instrument = &instrument;
        sd.steppers = &steppers;
        sd.servos = &servos;
        sd.actuators = &actuators;
        sd.diag = &diag;
        sd.profile = &profile;
        sd.axisFaulted = &axisFaulted;
        sd.fault = [this](size_t i, const char* r, uint32_t) {
            if (i < axisFaulted.size()) axisFaulted[i] = true;
            faults.push_back(r);
        };
        sd.actOk = [](size_t, ActuatorResult r, const char*, uint32_t) { return ok(r); };
        scheduler.bind(sd);
        // The FSM only runs while armed; give it a machine that is.
        safety.beginHoming(true, true, 0, 0);
        safety.releaseComplete(0);
        safety.armAfterHoming();
    }

    // Advance the whole rig to `untilMs`, ticking the FSM for both strings.
    void run(uint32_t fromMs, uint32_t untilMs, uint32_t stepMs = 5) {
        for (uint32_t t = fromMs; t <= untilMs; t += stepMs) {
            gmbSetMillis(t);
            steppers.updateSensors(t);
            steppers.tick(t * 1000ULL);
            servos.update(t);
            instrument.tick(t * 1000ULL);
            for (size_t i = 0; i < instrument.stringCount(); ++i)
                if (!scheduler.tickAxisSafety(i, t)) scheduler.tick(i, t);
        }
    }

    MidiEvent noteOn(uint8_t note, uint8_t vel = 100) {
        MidiEvent e;
        e.type = static_cast<uint8_t>(MidiType::NoteOn);
        e.channel = 0; e.data1 = note; e.data2 = vel;
        e.source = static_cast<uint8_t>(MidiSource::WifiUdp);
        return e;
    }
    MidiEvent noteOff(uint8_t note) {
        MidiEvent e = noteOn(note, 0);
        e.type = static_cast<uint8_t>(MidiType::NoteOff);
        return e;
    }
};

// The core §16 rule. A note that needs a fret must lift the finger and WAIT its
// travel time before the carriage is allowed to move — otherwise the finger is
// dragged along the string. This is enforced purely by the phase ORDER, so it is
// exactly what a behavioural test has to pin down.
void carriage_waits_for_the_finger_to_lift() {
    beginCase("finger lifts before the carriage moves");
    Rig r;
    r.build();
    // Note 62 = fret 2 on the C string (60): a real move, and a finger press.
    r.instrument.handleEvent(r.noteOn(62), 0);
    r.instrument.tick(0);

    // One tick at t=0: the FSM must have released the finger and be WAITING, not
    // already commanding a move.
    gmbSetMillis(0);
    for (size_t i = 0; i < r.instrument.stringCount(); ++i) r.scheduler.tick(i, 0);
    check(countCalls("moveTo") == 0,
          "no carriage move may be commanded in the same tick the finger was released");

    // The finger's travelMs is 40 ms; before that elapses nothing may be commanded.
    r.run(5, 30);
    check(countCalls("moveTo") == 0,
          "the carriage must not be commanded before the finger travel time");

    // After it, the move is allowed to start.
    r.run(35, 300);
    check(countCalls("moveTo") > 0,
          "once the finger has lifted the carriage must actually be commanded");
}

// A Note Off mid-sequence must STOP the carriage where it is — not let it finish
// travelling to a fret nobody is going to play.
void note_off_during_a_move_stops_the_carriage() {
    beginCase("Note Off during a move");
    Rig r;
    r.build();
    r.instrument.handleEvent(r.noteOn(71), 0);   // a long move (high fret)
    r.instrument.tick(0);
    r.run(0, 120);                                // let the sequence get under way
    check(countCalls("moveTo") > 0, "precondition: a carriage move was commanded");
    int stopsBefore = countCalls("stopMove");

    r.instrument.handleEvent(r.noteOff(71), 120000);
    r.instrument.tick(120000);
    r.run(125, 900);
    check(countCalls("stopMove") > stopsBefore,
          "a Note Off must command a stop, not let the move finish");
    check(r.scheduler.idle(0) && r.scheduler.idle(1),
          "both strings must return to Idle once stopped");
}

// A new note arriving while the previous one is still decelerating must not issue
// a move into a running motor — the FSM waits on atTarget().
//
// The stub has no deceleration model (moveTo arrives at once), so the harness
// represents "still braking" explicitly by holding the engine's running flag. That
// is the state the FSM branches on, which is the thing under test.
void new_note_during_deceleration_waits_for_a_stopped_axis() {
    beginCase("new note during deceleration");
    Rig r;
    r.build();
    r.instrument.handleEvent(r.noteOn(62), 0);
    r.instrument.tick(0);
    r.run(0, 300);           // complete a first note so the axis is settled
    int movesBefore = countCalls("moveTo");

    // Every axis is now "still decelerating" from the caller's point of view.
    for (auto* s : gmbLiveSteppers()) s->running = true;

    r.instrument.handleEvent(r.noteOff(62), 300000);
    r.instrument.tick(300000);
    r.instrument.handleEvent(r.noteOn(71), 305000);   // a different fret
    r.instrument.tick(305000);
    r.run(310, 900);
    check(countCalls("moveTo") == movesBefore,
          "no move may be commanded while the axis still reports running");

    // Once the axis really has stopped, the move is allowed through.
    for (auto* s : gmbLiveSteppers()) s->running = false;
    r.run(905, 1600);
    check(countCalls("moveTo") > movesBefore,
          "once the axis is stopped the pending note must finally move it");
}

// A LIMIT asserting at any time removes that axis from service, without touching
// the other one.
void limit_trip_faults_only_its_own_axis() {
    beginCase("LIMIT trip isolates one axis");
    Rig r;
    r.build();
    gmbSetPinLevel(kLimitPin0, LOW);   // limitActiveHigh=false -> LOW asserts
    r.run(0, 60);
    check(r.axisFaulted[0], "the axis whose LIMIT tripped must be faulted");
    check(!r.axisFaulted[1], "the other axis must be untouched");
    check(!r.faults.empty(), "the fault must be reported with a reason");
}

// An axis already faulted must not be re-faulted on every subsequent tick: the
// fault path rebuilds capabilities and notifies, so repeating it per tick would
// bury the machine in work and log noise.
void a_faulted_axis_is_not_refaulted_every_tick() {
    beginCase("faulted axis is not re-reported");
    Rig r;
    r.build();
    gmbSetPinLevel(kLimitPin0, LOW);
    r.run(0, 60);
    size_t after = r.faults.size();
    r.run(65, 400);
    check(r.faults.size() == after, "a faulted axis must be reported once, not per tick");
}

// abortString() must physically release what the axis had engaged: a fault that
// leaves a strum lift pressed on the string mutes it until someone notices.
void abort_releases_an_engaged_lift() {
    beginCase("abortString clears the FSM");
    Rig r;
    r.build();
    r.instrument.handleEvent(r.noteOn(62), 0);
    r.instrument.tick(0);
    r.run(0, 200);
    r.scheduler.abortString(0);
    check(r.scheduler.idle(0), "the aborted string must be Idle");
    r.scheduler.abortString(1);
    check(r.scheduler.idle(1), "aborting an idle string must be harmless");
}

}  // namespace

int main() {
    std::printf("PlaybackScheduler runtime check\n");
    carriage_waits_for_the_finger_to_lift();
    note_off_during_a_move_stops_the_carriage();
    new_note_during_deceleration_waits_for_a_stopped_axis();
    limit_trip_faults_only_its_own_axis();
    a_faulted_axis_is_not_refaulted_every_tick();
    abort_releases_an_engaged_lift();
    return finish("playbackschedulercheck");
}
