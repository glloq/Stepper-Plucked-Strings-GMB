// SafetySupervisor: the arming / homing / hard-stop / panic / runtime-fault
// operations, lifted out of main.cpp so the P0 safety sequence lives in one named
// unit instead of a scatter of free functions in the middle of a sketch.
//
// It OWNS no state. Every collaborator — the SafetyManager, the actuator stack
// (StepperBank / ServoBank / InstrumentController / PlaybackScheduler), the homing
// controllers, the shared app phase, the degraded flag, the per-axis fault and
// anchor vectors, the E-stop pin and the active profile — is injected by pointer
// through bind(), and the cross-domain steps are injected as callbacks:
//
//   rebuildCaps       rebuild the capability snapshot -> working axis count. Needs
//                     the SysEx service and the state mutex, which stay in main.
//   notifyCaps        push the "capabilities changed" SysEx to the last host.
//   hardStopCleanup   drop scheduled test Note Offs and cancel a pending profile
//                     activation — app bookkeeping, not mechanics.
//   onReady           the machine just reached Ready (settle a deferred activation).
//
// So the storage and every existing reader in main.cpp stay exactly where they are:
// only the operation BODIES moved here, unchanged, and main.cpp's free functions
// became thin delegates so every call site is byte-identical.
//
// Arduino-gated (it drives StepperBank / ServoBank and reads a GPIO), so it is not
// reachable from the pure-core unit tests. It is NOT untested for that reason:
// test/runtimecheck/ builds this component against the real banks on instrumented
// stubs and drives the sequences — E-stop at pre-arm on both polarities, LIMIT
// during the seek, zero axes homed, the last axis failing, an unattributable PCA
// loss, hard stop, and the reset preconditions. Each guard there is covered by a
// case that fails when the guard is removed.
//
// The E-stop polarity predicate additionally lives in the pure core as
// core/safety/EstopPolarity.h, where it is unit-tested for both wirings —
// precisely because getting it backwards is how a healthy machine refuses to home
// and a pressed button goes unseen.
//
// What none of that proves: timing on real silicon, I2C under load, or whether a
// finger physically clears a string. Those remain bench work.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../../core/app/AppPhase.h"
#include "../../core/configuration/Profile.h"
#include "../../core/configuration/ProfileValidator.h"
#include "../../core/diagnostics/Diagnostics.h"
#include "../../core/instrument/InstrumentController.h"
#include "../../core/motion/HomingController.h"
#include "../../core/safety/EstopPolarity.h"
#include "../../core/safety/SafetyManager.h"
#include "PlaybackScheduler.h"
#include "ServoBank.h"
#include "StepperBank.h"

#if defined(ARDUINO)
#include <Arduino.h>  // digitalRead / millis / HIGH
#endif

namespace gmb {

class SafetySupervisor {
public:
    struct Deps {
        SafetyManager* safety = nullptr;
        StepperBank* steppers = nullptr;
        ServoBank* servos = nullptr;
        InstrumentController* instrument = nullptr;
        PlaybackScheduler* scheduler = nullptr;
        Diagnostics* diag = nullptr;
        Profile* profile = nullptr;
        AppPhase* phase = nullptr;
        bool* degraded = nullptr;
        bool* homingStarted = nullptr;
        std::vector<HomingController>* homing = nullptr;
        std::vector<bool>* anchored = nullptr;
        std::vector<bool>* axisFaulted = nullptr;
        int8_t* estopPin = nullptr;
        std::function<int()> rebuildCaps;       // -> working axis count
        std::function<void()> notifyCaps;
        std::function<void()> hardStopCleanup;  // test Note Offs + pending activation
        std::function<void()> onReady;          // reached Ready (settle a deferred swap)
    };

    void bind(const Deps& d) { d_ = d; }

    // Is the E-stop asserted, given the raw pin level (HIGH = true) and the profile's
    // declared wiring? Two polarities are supported:
    //   normally-OPEN  (legacy): a button shorts the pin to GND -> LOW means stop.
    //   normally-CLOSED (recommended, hardware/POWER_AND_SAFETY.md): a closed loop to
    //     GND holds the pin LOW to AUTHORISE running, so a press, a cut wire or an
    //     unplugged connector all read HIGH (pull-up) = stop. Losing the E-stop chain
    //     then fails safe instead of silently disarming the E-stop.
    bool estopAsserted(bool pinHigh) const {
        return estopAssertedFor(pinHigh, d_.profile->estopNormallyClosed);
    }

    // Read the E-stop pin and normalise it. EVERY check goes through this — the bug
    // this replaces was two call sites keeping their own `digitalRead(pin) == LOW`.
    // Returns false when no E-stop pin is configured (nothing to assert).
    bool estopAssertedNow() const {
        const int8_t pin = *d_.estopPin;
        if (pin < 0) return false;
        return estopAsserted(digitalRead(pin) == HIGH);
    }

    bool locked() const {
        SafetyState s = d_.safety->state();
        return s == SafetyState::Panic || s == SafetyState::EmergencyStop;
    }

    // Central runtime axis-fault path (LIMIT, motor/servo error, homing failure).
    void faultRuntimeAxis(size_t i, const char* reason, uint32_t nowMs) {
        InstrumentController& g_instrument = *d_.instrument;
        StepperBank& g_steppers = *d_.steppers;
        ServoBank& g_servos = *d_.servos;
        SafetyManager& g_safety = *d_.safety;
        PlaybackScheduler& g_scheduler = *d_.scheduler;
        std::vector<bool>& g_anchored = *d_.anchored;
        std::vector<bool>& g_axisFaulted = *d_.axisFaulted;

        if (i >= g_instrument.stringCount()) return;
        g_steppers.emergencyStop(i);
        g_instrument.faultString(i);
        // Physically release any servo this axis had engaged BEFORE wiping the sched,
        // so a single-axis fault never leaves the finger clamped or the strum lift
        // pressed on the string (finger/strum leads can engage them before arrival).
        // This mirrors the Note Off / note-replacement release paths.
        if (i < g_scheduler.size()) {
            int fi = g_servos.fingerIndex(static_cast<int>(i));
            if (fi >= 0) g_servos.release(fi);
            g_scheduler.abortString(i);  // raises an engaged strum lift, clears the FSM
        }
        if (i < g_anchored.size()) g_anchored[i] = false;
        if (i < g_axisFaulted.size()) g_axisFaulted[i] = true;
        g_safety.recordFault("axis", std::string(reason) + " on axis " + std::to_string(i),
                             nowMs);
        int working = d_.rebuildCaps();
        d_.notifyCaps();  // push the change notification so GMB learns of it
        // If the last operational axis just failed, the instrument can no longer play
        // anything: neutralise and latch a panic rather than sitting "armed" with zero
        // strings (which would also emit a bogus 0..0 capability range).
        if (working <= 0) {
            hardStopAll();
            g_safety.panic("no operational axes remain", nowMs);
        }
    }

    // A PCA9685 stopped ACKing at runtime (unplugged / brown-out). Rather than a global
    // panic, take out ONLY the strings that board actually drives (audit P1.5): the
    // instrument keeps playing degraded on the survivors and re-advertises its real
    // capabilities. A panic is still the right answer when the loss cannot be isolated:
    //   * the board drives no string at all -> it is a shared resource (a shared damper,
    //     an aux actuator) whose failure is not attributable, so fail safe; or
    //   * no working axis remains -> faultRuntimeAxis panics on its own.
    void serviceLostPcaBoard(uint8_t failedBoard, uint32_t nowMs) {
        InstrumentController& g_instrument = *d_.instrument;
        ServoBank& g_servos = *d_.servos;
        SafetyManager& g_safety = *d_.safety;
        std::vector<bool>& g_axisFaulted = *d_.axisFaulted;

        std::string where = ServoBank::boardName(failedBoard);
        int hit = 0;
        for (size_t i = 0; i < g_instrument.stringCount(); ++i) {
            if (i < g_axisFaulted.size() && g_axisFaulted[i]) continue;
            if (!g_servos.stringUsesBoard(static_cast<int>(i), failedBoard)) continue;
            ++hit;
            faultRuntimeAxis(i, ("PCA9685 lost on " + where).c_str(), nowMs);
        }
        if (hit == 0) {
            // Not attributable to any string: fail safe.
            g_safety.recordFault("servo",
                                 "PCA9685 on " + where + " stopped responding (no string "
                                 "mapped to it — failing safe)", nowMs);
            panic();
        }
    }

    // Start homing only when it is safe to move. Refuses if a panic/E-stop is
    // latched, if the profile is invalid, or if a required motor could not attach a
    // hardware step generator (spec §13/§21).
    bool beginHoming(uint32_t nowMs) {
        InstrumentController& g_instrument = *d_.instrument;
        StepperBank& g_steppers = *d_.steppers;
        ServoBank& g_servos = *d_.servos;
        SafetyManager& g_safety = *d_.safety;
        Profile& g_profile = *d_.profile;
        std::vector<HomingController>& g_homing = *d_.homing;
        std::vector<bool>& g_anchored = *d_.anchored;

        if (locked()) return false;
        // Never enable drivers while a hardware E-stop is physically asserted, even
        // if the software state has not caught up yet (closes the power-on window).
        // Through estopAssertedNow(), so the DECLARED polarity applies here too: this
        // check used to test LOW directly, which on the recommended normally-closed
        // chain is the HEALTHY level — it refused to home a safe machine and let a
        // genuinely pressed E-stop through.
        if (estopAssertedNow()) {
            g_safety.emergencyStop(nowMs);
            return false;
        }
        if (!ProfileValidator::isActivatable(g_profile)) return false;
        if (g_steppers.attachFault() || g_servos.directAttachFault() ||
            g_servos.pcaAttachFault()) {
            g_safety.recordFault("attach",
                                 "a motor/servo/PCA9685 could not attach or respond", nowMs);
            return false;
        }
        // Reconfiguration/homing must start from PowerOnSafe, never from Armed: a
        // stale Armed state would let /api/test/servo drive a servo mid-homing and
        // would make the final arming a silent no-op. locked() (Panic/E-stop)
        // is already refused above, so this only demotes a lingering Armed state.
        g_safety.reset();  // -> PowerOnSafe; tickHoming arms once the axes are homed
        *d_.degraded = false;
        g_steppers.enableDrivers(true);
        // Clear every latched PCA channel BEFORE /OE goes low, so enabling the outputs
        // cannot release a stale pulse on all channels at once (audit P0) — the servos
        // are then walked to rest progressively by the governed park below.
        g_servos.neutralizePcaOutputs();
        g_servos.outputEnable(true);

        for (size_t i = 0; i < g_homing.size(); ++i) {
            g_anchored[i] = false;
            if (!g_profile.strings[i].enabled) {
                g_instrument.string(i).disable();  // never home a disabled axis
                continue;
            }
            g_instrument.string(i).setHoming();
        }

        // CONTROLLED PARK of every servo (fingers up, strikers home) before any carriage
        // moves — a still-pressed finger must never drag along the string as the axis
        // seeks HOME (§16). The park is GOVERNED so arming does not fire every servo
        // simultaneously; beginGovernedPark returns the upper-bound wait, which becomes
        // the SafetyManager's release window. The homing controllers start only after it.
        uint32_t releaseWaitMs = g_servos.beginGovernedPark(
            nowMs, g_profile.power.maxConcurrentMoves, g_profile.power.maxConcurrentPerBoard,
            g_profile.power.staggerMs);
        *d_.homingStarted = false;
        if (!g_safety.beginHoming(true, true, releaseWaitMs, nowMs)) {
            // The safety FSM refused (latched state): leave the machine safe.
            g_servos.hardStop();
            g_steppers.enableDrivers(false);
            return false;
        }
        *d_.phase = AppPhase::Homing;
        return true;
    }

    // Whether any endstop is currently asserted (blocks a reset / re-home).
    bool anyLimitActive() const {
        StepperBank& g_steppers = *d_.steppers;
        for (size_t i = 0; i < g_steppers.count(); ++i)
            if (g_steppers.limitActive(i)) return true;
        return false;
    }

    // Explicit recovery after a panic / E-stop: only proceeds when the E-stop is
    // released, no LIMIT is asserted, the profile is valid and all channels attached.
    bool reset(uint32_t nowMs) {
        InstrumentController& g_instrument = *d_.instrument;
        StepperBank& g_steppers = *d_.steppers;
        ServoBank& g_servos = *d_.servos;
        SafetyManager& g_safety = *d_.safety;
        Profile& g_profile = *d_.profile;
        std::vector<bool>& g_axisFaulted = *d_.axisFaulted;

        if (estopAssertedNow()) return false;  // still asserted (either polarity)
        if (anyLimitActive()) return false;
        if (!ProfileValidator::isActivatable(g_profile)) return false;
        if (g_steppers.attachFault() || g_servos.directAttachFault() ||
            g_servos.pcaAttachFault())
            return false;
        g_safety.reset();                 // Panic/EStop -> PowerOnSafe
        g_safety.clearFaults();
        // Recover runtime-faulted axes so a reset can actually bring them back: clear
        // the fault flag AND the StringController fault, then re-home (audit P0-3).
        for (size_t i = 0; i < g_axisFaulted.size(); ++i) {
            if (g_axisFaulted[i]) {
                g_axisFaulted[i] = false;
                g_instrument.recoverString(i);  // Fault -> Idle, un-fault the allocator
            }
        }
        return beginHoming(nowMs);         // mandatory re-home before playing again
    }

    void tickHoming(uint32_t nowMs) {
        InstrumentController& g_instrument = *d_.instrument;
        StepperBank& g_steppers = *d_.steppers;
        ServoBank& g_servos = *d_.servos;
        SafetyManager& g_safety = *d_.safety;
        Diagnostics& g_diag = *d_.diag;
        Profile& g_profile = *d_.profile;
        std::vector<HomingController>& g_homing = *d_.homing;
        std::vector<bool>& g_anchored = *d_.anchored;
        std::vector<bool>& g_axisFaulted = *d_.axisFaulted;

        // Hold every axis still until the fingers have physically lifted, then start
        // the homing controllers (their internal timers begin here, not before).
        if (!*d_.homingStarted) {
            // Keep issuing the governed park's rest commands as their slots open.
            ServoBank::ParkResult park = g_servos.serviceGovernedPark(nowMs);
            if (!park.ok) {
                // A rest command never reached a servo: the park CANNOT be trusted, so a
                // finger may still be on the string. Refuse to seek — hard-stop and stay
                // safely out of Ready rather than dragging a pressed finger (audit P0).
                hardStopAll();
                g_safety.recordFault(
                    "park", std::string("servo ") + std::to_string(park.failedServo) +
                                " refused its rest command (" +
                                actuatorResultName(park.reason) + ") — homing aborted",
                    nowMs);
                g_safety.reset();
                *d_.phase = AppPhase::Boot;
                return;
            }
            // Both walls must fall: every rest command issued AND travelled (the bank's
            // own view) and the SafetyManager's release window elapsed.
            if (!g_servos.governedParkDone(nowMs)) return;
            if (!g_safety.releaseComplete(nowMs)) return;
            for (size_t i = 0; i < g_homing.size(); ++i)
                if (g_profile.strings[i].enabled) g_homing[i].start(nowMs);
            *d_.homingStarted = true;
        }

        bool allDone = true;
        int active = 0, faulted = 0;
        for (size_t i = 0; i < g_homing.size(); ++i) {
            if (!g_profile.strings[i].enabled) continue;  // disabled axes are skipped
            ++active;
            // Watch the LIMIT switch during EVERY homing phase: hitting the opposite
            // endstop means the HOME sensor was missed — hard-stop and fault the axis
            // instead of grinding on until the timeout / max distance.
            if (!g_homing[i].ready() && !g_homing[i].failed() &&
                g_steppers.limitActive(i)) {
                g_steppers.emergencyStop(i);
                g_homing[i].abort(HomingFault::LimitTriggered);
            }
            if (g_homing[i].failed()) {
                g_instrument.faultString(i);  // remove from allocator + selection too
                g_axisFaulted[i] = true;
                g_steppers.emergencyStop(i);  // HARD stop: a failed axis must not keep
                                              // decelerating past the arming instant
                g_diag.addHomingFailure();
                ++faulted;
                continue;
            }
            if (g_homing[i].ready()) {
                if (!g_anchored[i]) {
                    // Anchor the coordinate system so the home sensor is 0 mm.
                    g_steppers.setPositionReference(i, g_homing[i].restOffsetMm());
                    g_instrument.string(i).homingDone();
                    g_anchored[i] = true;
                }
                continue;
            }
            allDone = false;
            bool rawHigh = g_steppers.homeRawHigh(i);
            HomingCommand cmd = g_homing[i].update(nowMs, rawHigh, g_steppers.positionMm(i),
                                                   g_steppers.isRunning(i));
            switch (cmd.kind) {
                case MoveKind::Stop: g_steppers.stop(i); break;
                case MoveKind::MoveVelocity: g_steppers.setVelocityMm(i, cmd.velocityMmS); break;
                case MoveKind::MoveTo: g_steppers.moveToMmRaw(i, cmd.targetMm); break;
            }
        }
        if (!allDone) return;

        // Never arm while a faulted axis is still physically moving (shared ENABLE
        // stays live for all drivers): wait for every faulted axis to report stopped.
        for (size_t i = 0; i < g_homing.size(); ++i) {
            if (g_axisFaulted[i] && g_steppers.isRunning(i)) return;  // still braking
        }

        // Refuse to arm with zero working axes: hard-stop and stay safely in Boot.
        if (active - faulted <= 0) {
            hardStopAll();
            g_safety.recordFault("homing", "no axis could be homed — not arming", nowMs);
            g_safety.reset();
            *d_.phase = AppPhase::Boot;
            return;
        }

        // Homing -> Armed through the safety FSM (the profile was validated before the
        // seek began). If the FSM refuses (a panic latched mid-homing) stay out of Ready.
        if (!g_safety.armAfterHoming()) {
            hardStopAll();
            *d_.phase = AppPhase::Boot;
            return;
        }
        *d_.phase = AppPhase::Ready;
        // A deferred profile activation only "succeeds" when the NEW profile really
        // reaches Ready — not when the swap merely started (audit 7).
        d_.onReady();
        if (faulted > 0) {
            // Announce only the axes that actually work (spec §13.2).
            d_.rebuildCaps();
            d_.notifyCaps();
            g_safety.recordFault("homing",
                                 std::to_string(faulted) + " axis/axes failed homing "
                                 "(degraded run)", nowMs);
        } else {
            *d_.degraded = false;
        }
    }

    // HARD STOP (audit P0.3): E-stop / panic / unrecoverable fault. Everything is cut
    // AT ONCE — no mechanical movement is ever a precondition for stopping. This is
    // deliberately distinct from the CONTROLLED PARK used by a profile change / normal
    // stop (see controlledParkAll), which first drives the fingers up and waits.
    void hardStopAll() {
        d_.instrument->panic();
        d_.steppers->hardStop();   // force-stop every axis where it stands, ENABLE off
        d_.servos->hardStop();     // /OE off + direct PWM off, before any I2C traffic
        d_.scheduler->reset();
        // Drop scheduled test Note Offs so a stale one can't stop a future note with
        // the same channel/number (audit P1-4), and cancel a deferred activation: a
        // panic / E-stop supersedes it, and the waiting command is reported CANCELLED
        // so a client polling it stops immediately instead of waiting out a ghost.
        d_.hardStopCleanup();
        *d_.phase = AppPhase::Boot;
    }

    void panic() {
        hardStopAll();
        // A hardware E-stop outranks a software panic: never downgrade a latched
        // EmergencyStop to Panic (audit P1-17). The machine is already neutralised.
        if (d_.safety->state() != SafetyState::EmergencyStop)
            d_.safety->panic("web/CC panic", millis());
    }

    // Hardware E-stop: latches the distinct EmergencyStop state (not Panic).
    void emergencyStop() {
        hardStopAll();
        d_.safety->emergencyStop(millis());
    }

private:
    Deps d_;
};

}  // namespace gmb
