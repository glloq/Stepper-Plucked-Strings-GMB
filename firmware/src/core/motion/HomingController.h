// Non-blocking homing state machine (spec section 13).
//
// One controller per axis. It never blocks: each tick it reads the sensor and
// the current position, and returns the motion command the axis should apply.
// Faults disable the axis without disturbing the others.
#pragma once

#include <cstdint>

namespace gmb {

enum class HomingState : uint8_t {
    Idle,
    CheckSensor,
    ReleaseAtStart,  // sensor already active at start: back off until it clears
    SeekFast,
    BrakeFast,   // stop and wait for standstill before reversing (backoff)
    Backoff,
    SeekSlow,
    BrakeSlow,   // stop and wait for standstill before setting zero
    SetZero,
    MoveToOffset,
    Ready,
    Fault,
};

enum class HomingFault : uint8_t {
    None,
    SensorActiveAtStart,
    SensorNotReleased,
    SensorNeverReached,
    Timeout,
    MaxDistanceExceeded,
    LimitTriggered,  // opposite LIMIT switch hit during the homing seek
};

// What kind of device the endstop physically is.
//
// This is NOT a cosmetic label. A mechanical contact bounces — the moving blade
// makes and breaks several times over a few milliseconds — so its level has to
// settle before it can be believed. An optical gate has no contact to bounce: its
// output is a comparator edge, and the settling time that protects a switch is
// pure lag on a slotted sensor.
//
// That lag is not harmless here. The zero is latched when the sensor trips during
// the SLOW seek, so a debounce of d milliseconds at a slow-seek speed of v mm/s
// places the zero `v·d/1000` mm past where the sensor actually tripped. At the
// default 5 mm/s and 3 ms that is 15 µm — small, but it is a systematic offset
// that MOVES when the slow-seek speed is retuned, which is the confusing kind of
// error: change a speed, and every fret shifts. Fitting an optical endstop for
// precision and then filtering its edge for 3 ms throws away most of what was
// bought, so the type selects the filter.
enum class EndstopType : uint8_t { Mechanical = 0, Optical = 1 };

// A mechanical contact settles in ~1–2 ms on a good microswitch; 3 ms is the
// conservative value this firmware has always used. An optical gate is taken at
// face value: the sampling loop already runs at loop() rate, and the ESP32 input
// is Schmitt-triggered, so a clean comparator output needs no extra filter.
constexpr uint8_t endstopDebounceMs(EndstopType t) {
    return t == EndstopType::Optical ? 0 : 3;
}

// The canonical on-disk / on-wire name. Lives here rather than in the JSON layer
// because the endstop test endpoint reports it too, and one spelling shared by the
// profile and the API is one fewer thing to keep in step.
constexpr const char* endstopTypeName(EndstopType t) {
    return t == EndstopType::Optical ? "optical" : "mechanical";
}

struct HomingConfig {
    int8_t direction = -1;      // +1 or -1: travel direction toward the sensor
    double fastSpeedMmS = 40.0;
    double slowSpeedMmS = 5.0;
    double backoffMm = 3.0;
    double offsetMm = 0.0;      // final resting offset past the zero
    uint32_t timeoutMs = 8000;
    double maxSearchMm = 500.0;
    bool sensorActiveHigh = true;   // HOME sensor polarity
    bool limitActiveHigh = false;   // LIMIT switch polarity (independent of HOME)
    // Sensor technology, per endstop. They are independent on purpose: the most
    // common upgrade is an optical HOME (it sets the zero, so its repeatability is
    // the axis's repeatability) kept alongside a cheap mechanical LIMIT, which only
    // ever has to say "you have gone too far".
    EndstopType homeSensor = EndstopType::Mechanical;
    EndstopType limitSensor = EndstopType::Mechanical;
};

// How far past the true trigger point the zero lands, purely because the HOME level
// had to settle first. Reported by the UI so the cost of the filter is a number the
// operator can see rather than a claim.
inline double homingSensorLagMm(const HomingConfig& c) {
    double v = c.slowSpeedMmS < 0 ? -c.slowSpeedMmS : c.slowSpeedMmS;
    return v * endstopDebounceMs(c.homeSensor) / 1000.0;
}

enum class MoveKind : uint8_t { Stop, MoveVelocity, MoveTo };

struct HomingCommand {
    MoveKind kind = MoveKind::Stop;
    double velocityMmS = 0.0;  // signed, for MoveVelocity
    double targetMm = 0.0;     // for MoveTo
};

class HomingController {
public:
    void configure(const HomingConfig& cfg) { cfg_ = cfg; }
    void start(uint32_t nowMs);

    // Advance the machine. `rawSensor` is the electrical level (normalised
    // through sensorActiveHigh). `isMoving` is the motor's real running state
    // (from the step engine): the brake phases wait for it to become false
    // before commanding a reversal, so a reverse is never issued mid-motion.
    HomingCommand update(uint32_t nowMs, bool rawSensor, double currentPosMm,
                         bool isMoving);

    // Force this axis into a homing fault (e.g. a LIMIT switch tripped during the
    // seek). The caller is responsible for the emergency stop of the motor.
    void abort(HomingFault f) {
        state_ = HomingState::Fault;
        fault_ = f;
    }

    HomingState state() const { return state_; }
    HomingFault fault() const { return fault_; }
    bool ready() const { return state_ == HomingState::Ready; }
    bool failed() const { return state_ == HomingState::Fault; }

    // Where the axis ends up once homed, expressed relative to the home sensor
    // (= 0). The integrator uses it to anchor the axis coordinate system so the
    // home point is 0 mm.
    double restOffsetMm() const {
        double dir = cfg_.direction >= 0 ? 1.0 : -1.0;
        return -dir * cfg_.offsetMm;
    }

private:
    HomingConfig cfg_;
    HomingState state_ = HomingState::Idle;
    HomingFault fault_ = HomingFault::None;
    uint32_t startMs_ = 0;
    double startPosMm_ = 0.0;
    double triggerPosMm_ = 0.0;        // fast-seek trigger (for back-off distance)
    double sensorTriggerPosMm_ = 0.0;  // slow-seek trigger = the zero reference
    double backoffTargetMm_ = 0.0;
    double offsetTargetMm_ = 0.0;

    bool active(bool raw) const { return raw == cfg_.sensorActiveHigh; }
    HomingCommand fail(HomingFault f);
    bool timedOut(uint32_t nowMs) const {
        return (nowMs - startMs_) > cfg_.timeoutMs;
    }
};

}  // namespace gmb
