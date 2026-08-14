// Per-string mechanical playback scheduler.
//
// This is the non-blocking FSM that used to live in main.cpp::tickString(), plus the
// per-axis endstop scan that runs ahead of it. Moving it into its own component
// shrinks main.cpp toward `begin()/tick()` and gives the mechanical sequencing —
// the part that decides when a carriage moves, when a finger presses and when a
// string is struck — one named home instead of 350 lines in the middle of a sketch.
//
// The FSM logic is UNCHANGED. Each method aliases its injected collaborators back to
// the historical `g_*` names on its first lines, so the bodies below are byte-identical
// to the previous free functions; only the wiring moved. That is deliberate: this is a
// mechanical path on a machine with 24 V steppers, and a refactor is not the place to
// also "improve" the sequencing.
//
// PlaybackScheduler owns the per-string FSM state (StringSched) and nothing else. Its
// collaborators (InstrumentController, StepperBank, ServoBank, ActuatorManager,
// Diagnostics, the active Profile) are injected once via bind(); two callbacks let it
// fault an axis through the app's central fault path without needing to know about
// capability rebuilds or panic escalation.
//
// Arduino-gated by its collaborators (StepperBank / ServoBank): compile-checked by
// hostcheck, NOT host-unit-tested. Behaviour is preserved by construction (verbatim
// bodies); like every mechanical path in this repo it is to be RE-VALIDATED on the
// bench, not assumed correct because the software builds.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "../../core/configuration/Profile.h"
#include "../../core/diagnostics/Diagnostics.h"
#include "../../core/instrument/ActuatorManager.h"
#include "../../core/instrument/ActuatorResult.h"
#include "../../core/instrument/InstrumentController.h"
#include "ServoBank.h"
#include "StepperBank.h"

namespace gmb {

// Per-string non-blocking playback scheduler state (moved verbatim from main.cpp).
struct StringSched {
    enum Phase { Idle, WaitStopped, ReleasingFinger, MovingToFret, PressingFinger,
                 Settling, Ready, StrumLiftDown, StrumLiftHold }
        phase = Idle;
    uint32_t phaseStartMs = 0;
    uint32_t commandId = 0;
    int fingerIndex = -1;
    uint32_t dampUntilMs = 0;   // don't move until the damper has acted (replace)
    uint32_t moveDeadlineMs = 0;  // fault the axis if the move isn't done by then
    int liftIndex = -1;    // engaged strum-lift servo during a stroke (-1 = none)
    int strikeIndex = -1;  // striker to fire once the lift has lowered
    uint32_t executeAtMs = 0;   // earliest time the note may sound (fixed delay)
    bool executeAnchored = false;  // executeAtMs fixed at the Note-On instant
    uint32_t estArriveMs = 0;   // estimated carriage arrival time (finger lead)
    bool fingerPressStarted = false;  // finger descent already begun (lead)
    uint32_t liftStartMs = 0;   // when the strum lift began lowering
    bool liftStarted = false;   // strum lift descent already begun (lead)
    uint32_t jogSafeAtMs = 0;   // earliest a manual jog may move (finger lifted)
};

// Estimate a trapezoidal/triangular move time (ms) for a distance at a given max
// speed (mm/s) and acceleration (mm/s^2). Used to set a move watchdog that scales
// with the profile instead of a fixed 4 s (audit P1-1).
//
// Free and pure, so it is usable (and checkable) without a scheduler instance.
inline uint32_t estimateMoveMs(double distanceMm, double vMaxMmS, double aMmS2) {
    distanceMm = std::fabs(distanceMm);
    if (distanceMm < 1e-6) return 0;
    if (vMaxMmS < 1e-6 || aMmS2 < 1e-6) return 0;  // caller applies a floor anyway
    double tAccel = vMaxMmS / aMmS2;              // time to reach cruise
    double dAccel = vMaxMmS * vMaxMmS / aMmS2;    // distance for accel + decel
    double t;
    if (distanceMm >= dAccel) {
        t = 2.0 * tAccel + (distanceMm - dAccel) / vMaxMmS;  // trapezoid
    } else {
        t = 2.0 * std::sqrt(distanceMm / aMmS2);             // triangle
    }
    return static_cast<uint32_t>(t * 1000.0);
}

class PlaybackScheduler {
public:
    // fault(i, reason, nowMs): route a runtime axis fault through the app's central
    // path (which itself calls abortString() back on this scheduler).
    using FaultFn = std::function<void(size_t, const char*, uint32_t)>;
    // actOk(axis, result, what, nowMs): a refused actuator write faults the axis and
    // returns false so the caller aborts this tick's musical step.
    using ActOkFn = std::function<bool(size_t, ActuatorResult, const char*, uint32_t)>;

    struct Deps {
        InstrumentController* instrument = nullptr;
        StepperBank* steppers = nullptr;
        ServoBank* servos = nullptr;
        ActuatorManager* actuators = nullptr;
        Diagnostics* diag = nullptr;
        const Profile* profile = nullptr;
        std::vector<bool>* axisFaulted = nullptr;
        FaultFn fault;
        ActOkFn actOk;
    };

    void bind(const Deps& d) { d_ = d; }

    // (Re)size the per-string state on a profile (re)load.
    void configure(size_t stringCount) { sched_.assign(stringCount, StringSched{}); }

    // Full reset of the FSM (panic / hard stop / profile swap).
    void reset() { for (auto& s : sched_) s = StringSched{}; }

    size_t size() const { return sched_.size(); }

    // Tear one string's sequence down after a runtime fault: raise any engaged strum
    // lift, then clear its state so nothing resumes mid-stroke.
    void abortString(size_t i) {
        if (i >= sched_.size()) return;
        if (sched_[i].liftIndex >= 0 && d_.servos) d_.servos->release(sched_[i].liftIndex);
        sched_[i] = StringSched{};
    }

    // A manual jog is only allowed on an idle axis whose finger has fully lifted
    // (§16: never drag the finger).
    bool idle(size_t i) const {
        return i < sched_.size() && sched_[i].phase == StringSched::Idle;
    }
    bool jogSafe(size_t i, uint32_t nowMs) const {
        if (i >= sched_.size()) return true;
        return static_cast<int32_t>(nowMs - sched_[i].jogSafeAtMs) >= 0;
    }

    // Per-axis endstop safety scan, run for EVERY axis each tick BEFORE any musical
    // logic and regardless of whether a note is active — so a carriage still
    // decelerating after a Note Off (or drifting while idle) is caught. Returns true
    // if the axis just faulted (caller should skip its musical tick).
    bool tickAxisSafety(size_t i, uint32_t nowMs) {
        StepperBank& g_steppers = *d_.steppers;
        Diagnostics& g_diag = *d_.diag;
        const std::vector<bool>& g_axisFaulted = *d_.axisFaulted;
        const FaultFn& faultRuntimeAxis = d_.fault;

        if (i < g_axisFaulted.size() && g_axisFaulted[i]) return false;  // already out
        // A LIMIT switch tripped removes this axis from service (allocator + selection
        // + capabilities) without disturbing the others.
        if (g_steppers.limitActive(i)) {
            g_diag.addLimitTrip();
            faultRuntimeAxis(i, "LIMIT tripped", nowMs);
            return true;
        }
        // HOME asserting while the carriage position says it is well away from home is
        // a position/reference fault (lost steps, drift, stuck/inverted sensor). Home
        // is anchored to 0 mm, so the ABSOLUTE distance is the mismatch (the axis
        // coordinate can run negative depending on the homing direction). Positions
        // legitimately near home (open string / low frets) are not faulted.
        static constexpr double kHomeMismatchMm = 10.0;
        if (g_steppers.homeActive(i) && std::fabs(g_steppers.positionMm(i)) > kHomeMismatchMm) {
            faultRuntimeAxis(i, "HOME asserted away from home (position drift)", nowMs);
            return true;
        }
        return false;
    }

    // Drive one string's mechanical sequence toward a plucked note.
    void tick(size_t i, uint32_t nowMs) {
        InstrumentController& g_instrument = *d_.instrument;
        StepperBank& g_steppers = *d_.steppers;
        ServoBank& g_servos = *d_.servos;
        ActuatorManager& g_actuators = *d_.actuators;
        Diagnostics& g_diag = *d_.diag;
        const Profile& g_profile = *d_.profile;
        std::vector<StringSched>& g_sched = sched_;
        const FaultFn& faultRuntimeAxis = d_.fault;
        const ActOkFn& actOk = d_.actOk;

        StringController& sc = g_instrument.string(i);
        const StringTarget& tgt = g_instrument.target(i);
        StringSched& sch = g_sched[i];

        if (!tgt.active) {
            if (sch.phase == StringSched::Idle) return;
            if (sch.phase != StringSched::WaitStopped) {
                // Note released / cancelled: STOP the carriage (a Note Off during a
                // move must not let it finish travelling), lift the finger, damp.
                g_steppers.stop(i);
                int fi = g_servos.fingerIndex(static_cast<int>(i));
                if (fi >= 0) g_servos.release(fi);
                // A manual jog must not move the carriage until the finger has fully
                // lifted off the string (§16: never drag the finger).
                sch.jogSafeAtMs = nowMs + g_servos.travelMs(fi);
                if (sch.liftIndex >= 0) { g_servos.release(sch.liftIndex); sch.liftIndex = -1; }
                sch.liftStarted = false;
                int di = g_servos.damperIndex(static_cast<int>(i));
                if (di >= 0) g_servos.strike(di);
                sch.phase = StringSched::WaitStopped;
            }
            // Only declare the axis idle once the carriage has REALLY stopped, so a
            // note accepted right after can't issue a moveTo into a still-decelerating
            // motor (the musical analogue of the homing brake states).
            if (!g_steppers.isRunning(i)) {
                sc.dampingDone();
                sch.phase = StringSched::Idle;
                sch.commandId = 0;
            }
            return;
        }

        if (sch.commandId != tgt.commandId) {
            // New note replacing a previous one on this string (e.g. the allocator's
            // ReplaceOldest): explicitly damp the still-vibrating string AND wait for
            // the damper's travel/settle before the carriage moves, so a ringing
            // string is not dragged to a new fret and re-plucked (audit P1-7).
            sch.dampUntilMs = nowMs;
            if (sch.phase != StringSched::Idle && sch.phase != StringSched::WaitStopped) {
                int di = g_servos.damperIndex(static_cast<int>(i));
                if (di >= 0) {
                    g_servos.strike(di);
                    sch.dampUntilMs = nowMs + g_servos.travelMs(di) + g_servos.settleMs(di);
                }
            }
            // A strum lift engaged for the previous note is raised before anything else.
            if (sch.liftIndex >= 0) { g_servos.release(sch.liftIndex); sch.liftIndex = -1; }
            // Fixed reception -> sound delay. A directly-played note is received now,
            // so anchor the delay here. A merely-PREPARED (anticipated) note is not
            // "received" until its Note On triggers it, so leave it unanchored and
            // re-anchor at the trigger instant in the Ready state below.
            sch.executeAtMs = nowMs + g_profile.midi.noteExecutionDelayMs;
            sch.executeAnchored = sc.willArmOnSettle();
            sch.fingerPressStarted = false;
            sch.liftStarted = false;
            sch.strikeIndex = -1;
            // Lift the finger and WAIT for it to travel up before moving the carriage,
            // so the finger never drags along the string (§16).
            sch.commandId = tgt.commandId;
            sch.fingerIndex = g_servos.fingerIndex(static_cast<int>(i));
            if (sch.fingerIndex >= 0) g_servos.release(sch.fingerIndex);
            sch.phase = StringSched::ReleasingFinger;
            sch.phaseStartMs = nowMs;
        }

        // A prepared (anticipated) note is "received" when its Note On triggers it —
        // possibly while it is still mechanically moving. Anchor the fixed execution
        // delay to that trigger instant so it is not over-delayed to settle time.
        if (!sch.executeAnchored && sc.consumeTriggerEdge()) {
            sch.executeAtMs = nowMs + g_profile.midi.noteExecutionDelayMs;
            sch.executeAnchored = true;
        }

        switch (sch.phase) {
            case StringSched::ReleasingFinger:
                // Start moving only once the finger has lifted, the damper (if any) has
                // acted, AND the carriage has fully stopped (a new note arriving during
                // a cancel deceleration must not issue a moveTo into a moving motor).
                if ((sch.fingerIndex < 0 ||
                     nowMs - sch.phaseStartMs >= g_servos.travelMs(sch.fingerIndex)) &&
                    static_cast<int32_t>(nowMs - sch.dampUntilMs) >= 0 &&
                    g_steppers.atTarget(i)) {
                    // Starting a carriage is the other in-rush event on this machine
                    // (a whole chord accelerating at once), so a REPOSITIONING move is
                    // staggerable like a finger press. It has slack: the note only
                    // sounds at executeAtMs, and a deferred start just eats into that
                    // budget. Steppers are on their own drivers, hence no board bucket.
                    if (!g_actuators.requestMove(MoveClass::Staggerable, nowMs,
                                                 g_steppers.board(i)))
                        break;  // no permit this tick — retry on the next
                    double dist = tgt.positionMm - g_steppers.positionMm(i);
                    const AxisConfig& ac = g_profile.strings[i];
                    uint32_t est = estimateMoveMs(dist, ac.maxSpeedMmS, ac.maxAccelMmS2);
                    // Generous margin (3x + 500 ms floor) so only a genuine stall/refusal
                    // faults the axis, but a slow-but-valid profile is never mis-flagged.
                    if (!actOk(i, g_steppers.moveToMm(i, tgt.positionMm), "carriage move",
                               nowMs))
                        break;
                    sch.phase = StringSched::MovingToFret;
                    sch.phaseStartMs = nowMs;
                    sch.estArriveMs = nowMs + est;
                    sch.moveDeadlineMs = nowMs + 500u + 3u * est;
                }
                break;
            case StringSched::MovingToFret:
                // Finger lead: begin the finger descent up to fingerLeadMs before the
                // estimated arrival so the finger reaches the string around arrival,
                // trimming the post-arrival latency. Opt-in (0 = press only on arrival);
                // set too large it can drag, so it is the user's to tune.
                if (!sc.openString() && sch.fingerIndex >= 0 && !sch.fingerPressStarted &&
                    g_profile.midi.fingerLeadMs > 0 &&
                    static_cast<int32_t>(nowMs - sch.estArriveMs) +
                            static_cast<int32_t>(g_profile.midi.fingerLeadMs) >= 0) {
                    // A finger press is STAGGERABLE (audit P1.6): it draws its peak at
                    // start and has slack, so it waits for a governor permit. Being
                    // deferred a tick only trims the lead, never the note itself.
                    if (g_actuators.requestMove(MoveClass::Staggerable, nowMs,
                                                g_servos.board(sch.fingerIndex))) {
                        if (!actOk(i, g_servos.press(sch.fingerIndex), "finger press", nowMs))
                            break;
                        sch.fingerPressStarted = true;
                        sch.phaseStartMs = nowMs;  // finger travel timer starts now
                    }
                }
                // Arrived only when the carriage is stopped AND actually at the fret
                // position (a refused/interrupted move must not be read as "reached").
                if (g_steppers.reachedTarget(i)) {
                    sc.motionReached();
                    if (sc.openString() || sch.fingerIndex < 0) {
                        sch.phase = StringSched::Ready;  // no finger press for open string
                    } else if (sch.fingerPressStarted) {
                        // Finger already descending (lead) — keep its running travel
                        // timer (phaseStartMs) instead of restarting the press.
                        sch.phase = StringSched::PressingFinger;
                    } else if (actOk(i, g_servos.press(sch.fingerIndex), "finger press",
                                     nowMs)) {
                        // On ARRIVAL the press is on the critical path to the note, so it
                        // is issued straight away (registered as Deadline). The governor
                        // already spread the leads; delaying here would delay the sound.
                        g_actuators.requestMove(MoveClass::Deadline, nowMs,
                                                g_servos.board(sch.fingerIndex));
                        sch.phase = StringSched::PressingFinger;
                        sch.phaseStartMs = nowMs;
                    }
                    // A refused write already faulted the axis (P1.4/P1.5): play no note
                    // with a finger that never pressed.
                } else if (static_cast<int32_t>(nowMs - sch.moveDeadlineMs) >= 0) {
                    // The move never completed within its estimated budget (command
                    // refused by the step engine, a stall, or a stop far from target):
                    // fault the axis instead of waiting forever in MovingToFret.
                    g_diag.addMoveTimeout();
                    faultRuntimeAxis(i, "move did not reach target (timeout)", nowMs);
                }
                break;
            case StringSched::PressingFinger:
                if (nowMs - sch.phaseStartMs >= g_servos.travelMs(sch.fingerIndex)) {
                    sc.fingerPressed();
                    sch.phase = StringSched::Settling;
                    sch.phaseStartMs = nowMs;
                }
                break;
            case StringSched::Settling: {
                // Strum lead: begin lowering the strum lift up to strumLeadMs before the
                // string is Ready, so the strummer is already engaged when the strike
                // time comes (overlaps the lift travel with the finger settle). Skip it
                // for a merely-prepared note — it must not rest on (and mute) the string
                // through the whole pre-trigger window; its lift lowers after trigger.
                if (!sch.liftStarted && sc.willArmOnSettle() && g_profile.midi.strumLeadMs > 0) {
                    int pi = perStringStrikeIndex(i);
                    int li = pi >= 0 ? g_servos.strumLiftIndex(static_cast<int>(i)) : -1;
                    uint32_t settle = g_servos.settleMs(sch.fingerIndex);
                    if (li >= 0 &&
                        (nowMs - sch.phaseStartMs) + g_profile.midi.strumLeadMs >= settle &&
                        // Anticipated lift: staggerable, it has slack before the strike.
                        g_actuators.requestMove(MoveClass::Staggerable, nowMs,
                                                g_servos.board(li))) {
                        // start lowering the lift early
                        if (!actOk(i, g_servos.press(li), "strum lift engage", nowMs)) break;
                        sch.liftIndex = li;
                        sch.strikeIndex = pi;
                        sch.liftStartMs = nowMs;
                        sch.liftStarted = true;
                    }
                }
                if (nowMs - sch.phaseStartMs >= g_servos.settleMs(sch.fingerIndex)) {
                    sc.settled();
                    sch.phase = StringSched::Ready;
                }
                break;
            }
            case StringSched::Ready: {
                // An anticipated note is "received" when its Note On triggers it: the
                // fixed delay must run from that instant, not from prepare time. The
                // arm transitioning true here IS that trigger, so anchor now.
                if (!sch.executeAnchored && sc.pluckArmed()) {
                    sch.executeAtMs = nowMs + g_profile.midi.noteExecutionDelayMs;
                    sch.executeAnchored = true;
                }
                if (!sc.pluckArmed()) break;  // not armed (prepared / already plucked)
                int pi = perStringStrikeIndex(i);
                // Pre-lower the strum lift DURING the fixed-delay wait so the strike
                // lands AT executeAtMs even with a lift — this keeps a chord's lift and
                // no-lift strings synchronised. It begins travel+engageDelay before
                // executeAtMs; with a zero/short delay it simply starts as soon as ready.
                if (pi >= 0 && !sch.liftStarted) {
                    int li = g_servos.strumLiftIndex(static_cast<int>(i));
                    if (li >= 0) {
                        uint32_t liftMs = g_servos.travelMs(li) + g_servos.engageDelayMs(li);
                        if (static_cast<int32_t>(nowMs - sch.executeAtMs) +
                                static_cast<int32_t>(liftMs) >= 0) {
                            // This lift MUST be down by executeAtMs or the strike misses
                            // the string, so it is a Deadline move: registered for the
                            // in-rush picture but never throttled.
                            g_actuators.requestMove(MoveClass::Deadline, nowMs,
                                                    g_servos.board(li));
                            if (!actOk(i, g_servos.press(li), "strum lift engage", nowMs))
                                break;
                            sch.liftIndex = li;
                            sch.strikeIndex = pi;
                            sch.liftStartMs = nowMs;
                            sch.liftStarted = true;
                        }
                    }
                }
                // Fixed reception -> sound delay: stay ready but silent until the
                // scheduled execution time. The mechanics have been preparing (and any
                // anticipated strum lift has been lowering) during this window.
                if (static_cast<int32_t>(nowMs - sch.executeAtMs) < 0) break;
                if (!sc.executePluck(tgt.commandId)) break;
                // Per-string strike: every string is plucked/strummed on its own — there
                // is no shared strummer. An optional strum-lift lowers the strum servo
                // onto the string for the stroke, then raises it.
                if (pi >= 0) {
                    // Use the lift already lowering (strum lead / pre-lower) if any,
                    // otherwise start it now.
                    int li = sch.liftStarted ? sch.liftIndex
                                             : g_servos.strumLiftIndex(static_cast<int>(i));
                    if (li >= 0) {
                        if (!sch.liftStarted) {
                            // Sound is due now: never throttled (Deadline).
                            g_actuators.requestMove(MoveClass::Deadline, nowMs,
                                                    g_servos.board(li));
                            // lower / engage the strum servo now
                            if (!actOk(i, g_servos.press(li), "strum lift engage", nowMs))
                                break;
                            sch.liftIndex = li;
                            sch.strikeIndex = pi;
                            sch.liftStartMs = nowMs;
                            sch.liftStarted = true;
                        }
                        sch.phase = StringSched::StrumLiftDown;
                        break;
                    }
                    // The strike IS the sound: Deadline, never governed (audit P1.6).
                    g_actuators.requestMove(MoveClass::Deadline, nowMs, g_servos.board(pi));
                    if (!actOk(i, g_servos.strike(pi, tgt.intensity), "pluck strike", nowMs))
                        break;
                }
                break;
            }
            case StringSched::StrumLiftDown:
                // Strum once the lift has lowered the strum servo onto the string
                // (travel + engage delay from when the descent STARTED — which may have
                // been anticipated during the settle via strumLeadMs).
                if (static_cast<int32_t>(nowMs - (sch.liftStartMs +
                        g_servos.travelMs(sch.liftIndex) +
                        g_servos.engageDelayMs(sch.liftIndex))) >= 0) {
                    g_actuators.requestMove(MoveClass::Deadline, nowMs,
                                            g_servos.board(sch.strikeIndex));
                    if (!actOk(i, g_servos.strike(sch.strikeIndex, tgt.intensity),
                               "strum strike", nowMs))
                        break;
                    sch.phase = StringSched::StrumLiftHold;
                    sch.phaseStartMs = nowMs;
                }
                break;
            case StringSched::StrumLiftHold:
                // Hold the lift down until the strum stroke has completed, then raise it.
                if (nowMs - sch.phaseStartMs >= g_servos.travelMs(sch.strikeIndex)) {
                    g_servos.release(sch.liftIndex);  // raise / disengage
                    sch.liftIndex = -1;
                    sch.strikeIndex = -1;
                    sch.phase = StringSched::Ready;
                }
                break;
            case StringSched::WaitStopped:
            case StringSched::Idle:
                break;
        }
    }

private:
    // The striker for a string: its dedicated plucker if it has one, otherwise its
    // strummer. Every string strikes on its own — there is no shared strummer.
    int perStringStrikeIndex(size_t i) const {
        int p = d_.servos->pluckIndex(static_cast<int>(i));
        return p >= 0 ? p : d_.servos->strumIndex(static_cast<int>(i));
    }

    Deps d_;
    std::vector<StringSched> sched_;
};

}  // namespace gmb
