// Homogeneous actuator command result (audit P1.4).
//
// Every scheduler-facing servo motion returns one of these so the playback scheduler
// always knows whether a command actually reached the hardware — and, on failure,
// WHY — instead of some paths returning bool and others void. A non-Ok result is
// routed to the affected string (faulted + removed from MIDI allocation), the
// SafetyManager fault log, the web status and the serial log.
#pragma once

#include <cstdint>

namespace gmb {

enum class ActuatorResult : uint8_t {
    Ok = 0,
    InvalidIndex,       // no servo at this index
    Disabled,           // servo present but not enabled in the profile
    DriverUnavailable,  // the referenced PCA9685 board is not present / initialised
    BusFault,           // I2C write path unusable (e.g. a board stopped ACKing)
    OutputFault,        // direct-GPIO LEDC (re)attach/write failed, or no output pin
};

// Convenience predicate so call sites read `if (!ok(bank.press(i)))`.
inline bool ok(ActuatorResult r) { return r == ActuatorResult::Ok; }

inline const char* actuatorResultName(ActuatorResult r) {
    switch (r) {
        case ActuatorResult::Ok:                return "ok";
        case ActuatorResult::InvalidIndex:      return "invalid-index";
        case ActuatorResult::Disabled:          return "disabled";
        case ActuatorResult::DriverUnavailable: return "driver-unavailable";
        case ActuatorResult::BusFault:          return "bus-fault";
        case ActuatorResult::OutputFault:       return "output-fault";
    }
    return "unknown";
}

}  // namespace gmb
