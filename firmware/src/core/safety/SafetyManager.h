// Safety & fault handling (spec section 21).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gmb {

// Runtime safety state machine. The startup path is explicit (spec §13/§21, P0):
//
//   PowerOnSafe --(profile validated)--> Homing --(every axis anchored)--> Armed
//
// The `Homing` phase is the stepper analogue of the servo build's `Parking`: the
// outputs are live and the mechanics are travelling to a known reference, but the
// note engine and every manual mechanical test stay locked out until it completes.
// Unlike a servo park (a pure timer) homing is SENSOR-driven, so the transition to
// Armed is commanded by the caller (`armAfterHoming`) once every enabled axis has
// found HOME — with one timed sub-step kept here: the pre-seek finger release, which
// must elapse before any carriage is allowed to move (§16: never drag a finger).
//
// With NO valid profile the machine latches in ConfigSafe: the web/network come up
// so a profile can be built or loaded, but no actuator is ever driven and no MIDI
// reaches the mechanics. The firmware must never fabricate + arm a default profile
// on a real machine.
enum class SafetyState : uint8_t {
    ConfigSafe,     // no valid profile — actuators locked out, awaiting configuration
    PowerOnSafe,    // drivers off, servos neutralised, queues empty
    Homing,         // profile validated, outputs live, axes seeking their reference
    Armed,          // normal operation
    Panic,          // software panic latched
    EmergencyStop,  // hardware E-stop asserted
};

// NOTE: a `WifiLossBehavior` enum (FinishThenStop / StopImmediately / ContinueQueued
// / IdleKeepMotors) was removed here (audit P1.10). It was never wired to any config
// or profile field, while the runtime applied a single fixed policy — advertising a
// configurability that did not exist. The deliberate fixed policy today is: on link
// loss, cancel pending commands and release sounding notes but STAY ARMED (see
// main.cpp loop()). When the DeviceConfig/SafetyConfig split (P1.13) lands, a real
// wifiLoss policy field can be reintroduced here AND wired to the runtime together.

struct FaultRecord {
    std::string source;
    std::string message;
    uint32_t atMs = 0;
};

class SafetyManager {
public:
    SafetyState state() const { return state_; }

    // At power-up everything is neutralised, drivers disabled (spec 21.1).
    void boot() { state_ = SafetyState::PowerOnSafe; }

    // No valid profile is available: latch a safe state where actuators can never
    // move (actuatorsAllowed() stays false) but the web/network stay up so a profile
    // can be built or loaded. The firmware must NEVER auto-apply a fabricated
    // fallback profile to real hardware (P0 boot-safe).
    void configSafe() { state_ = SafetyState::ConfigSafe; }

    // Begin the homing sequence once the profile is validated and pins checked:
    //   PowerOnSafe -> Homing. `releaseWaitMs` is the mechanical time the caller must
    // allow for every finger servo to lift clear of the string before a carriage may
    // move — max(travelMs+settleMs) over the finger servos.
    // Refuses (stays put) unless the state is a clean PowerOnSafe; a panic / E-stop /
    // ConfigSafe must be reset() first. Returns true when homing has begun.
    bool beginHoming(bool profileValid, bool pinsValid, uint32_t releaseWaitMs,
                     uint32_t nowMs) {
        if (state_ != SafetyState::Homing && state_ != SafetyState::PowerOnSafe)
            return false;
        if (!profileValid || !pinsValid) return false;
        state_ = SafetyState::Homing;
        releaseStartMs_ = nowMs;
        releaseWaitMs_ = releaseWaitMs;
        return true;
    }

    // True once the pre-seek finger release has elapsed and the carriages may start
    // seeking. Always false outside Homing (nothing may move at all then).
    bool releaseComplete(uint32_t nowMs) const {
        if (state_ != SafetyState::Homing) return false;
        return static_cast<int32_t>(nowMs - (releaseStartMs_ + releaseWaitMs_)) >= 0;
    }

    // Milliseconds still to wait before the carriages may seek (0 outside Homing).
    uint32_t releaseRemainingMs(uint32_t nowMs) const {
        if (state_ != SafetyState::Homing) return 0;
        int32_t rem = static_cast<int32_t>((releaseStartMs_ + releaseWaitMs_) - nowMs);
        return rem > 0 ? static_cast<uint32_t>(rem) : 0;
    }

    // Homing -> Armed, commanded by the caller once every enabled axis is anchored
    // (or has been faulted out, as long as at least one still works). Returns true on
    // the transition; refuses in any other state so a stale call cannot arm a
    // panicked / E-stopped / unconfigured machine.
    bool armAfterHoming() {
        if (state_ != SafetyState::Homing) return false;
        state_ = SafetyState::Armed;
        return true;
    }

    // Direct arm, skipping the Homing phase (PowerOnSafe -> Armed). Kept for unit
    // tests and a zero-axis profile; the runtime uses beginHoming/armAfterHoming so
    // the mechanical reference is always established first.
    bool arm(bool profileValid, bool pinsValid) {
        if (state_ == SafetyState::PowerOnSafe && profileValid && pinsValid) {
            state_ = SafetyState::Armed;
            return true;
        }
        return false;
    }

    // Software panic (spec 21.3): the caller must flush the MIDI queue, cancel
    // motion/plucks, hard-stop the axes, lift fingers, neutralise servos, disable
    // drivers; this records the cause and latches the state.
    void panic(const std::string& cause, uint32_t nowMs);

    // Hardware emergency stop (spec 21.2).
    void emergencyStop(uint32_t nowMs);

    // Clear a latched panic / E-stop / ConfigSafe back to the safe state (a re-home
    // is then required to reach Armed).
    void reset() { state_ = SafetyState::PowerOnSafe; }

    // Are actuators allowed to respond to MIDI / play right now? Only when Armed —
    // ConfigSafe, PowerOnSafe and Homing all keep the note engine AND mechanical
    // tests locked out, so nothing plays before Ready.
    bool actuatorsAllowed() const { return state_ == SafetyState::Armed; }

    // True while the arming sequence is establishing the mechanical reference.
    bool homing() const { return state_ == SafetyState::Homing; }

    void recordFault(const std::string& source, const std::string& msg, uint32_t nowMs);
    const std::vector<FaultRecord>& faults() const { return faults_; }
    void clearFaults() { faults_.clear(); }
    // Cumulative number of faults recorded since boot — kept across clearFaults()
    // (which only trims the visible log) so /api/diagnostics reports a real total.
    uint32_t faultCount() const { return faultCount_; }

private:
    SafetyState state_ = SafetyState::PowerOnSafe;
    uint32_t releaseStartMs_ = 0;
    uint32_t releaseWaitMs_ = 0;
    uint32_t faultCount_ = 0;
    std::vector<FaultRecord> faults_;
};

}  // namespace gmb
