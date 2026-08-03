// Safety & fault handling (spec section 21).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gmb {

enum class SafetyState : uint8_t {
    PowerOnSafe,   // drivers off, servos neutralised, queues empty
    Armed,         // normal operation
    Panic,         // software panic latched
    EmergencyStop, // hardware E-stop asserted
};

// Configurable behaviour when Wi-Fi is lost (spec 21.4).
enum class WifiLossBehavior : uint8_t {
    FinishThenStop = 0,   // default: cancel pending, controlled release, READY
    StopImmediately = 1,
    ContinueQueued = 2,
    IdleKeepMotors = 3,
};

struct FaultRecord {
    std::string source;
    std::string message;
    uint32_t atMs = 0;
};

class SafetyManager {
public:
    SafetyState state() const { return state_; }

    // At power-up everything is neutralised (spec 21.1).
    void boot() { state_ = SafetyState::PowerOnSafe; }

    // Transition to normal operation only after the profile is validated and
    // GPIO checked.
    bool arm(bool profileValid, bool pinsValid) {
        if (state_ == SafetyState::PowerOnSafe && profileValid && pinsValid) {
            state_ = SafetyState::Armed;
            return true;
        }
        return false;
    }

    // Software panic (spec 21.3): the caller must flush the MIDI
    // queue, cancel motion/plucks, lift fingers, neutralise servos, disable
    // motors; this records the cause and latches the state.
    void panic(const std::string& cause, uint32_t nowMs);

    // Hardware emergency stop (spec 21.2).
    void emergencyStop(uint32_t nowMs);

    // Clear a latched panic/E-stop back to the safe state (requires re-arming).
    void reset() { state_ = SafetyState::PowerOnSafe; }

    // Are actuators allowed to move right now?
    bool actuatorsAllowed() const { return state_ == SafetyState::Armed; }

    void recordFault(const std::string& source, const std::string& msg, uint32_t nowMs);
    const std::vector<FaultRecord>& faults() const { return faults_; }
    void clearFaults() { faults_.clear(); }

private:
    SafetyState state_ = SafetyState::PowerOnSafe;
    std::vector<FaultRecord> faults_;
};

}  // namespace gmb
