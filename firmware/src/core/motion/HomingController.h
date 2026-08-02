// Non-blocking homing state machine (cahier des charges section 13).
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
};

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
};

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
