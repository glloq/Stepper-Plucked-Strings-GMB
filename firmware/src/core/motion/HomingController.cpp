#include "HomingController.h"

#include <cmath>

namespace gmb {

void HomingController::start(uint32_t nowMs) {
    state_ = HomingState::CheckSensor;
    fault_ = HomingFault::None;
    startMs_ = nowMs;
    startPosMm_ = 0.0;
}

HomingCommand HomingController::fail(HomingFault f) {
    state_ = HomingState::Fault;
    fault_ = f;
    return {MoveKind::Stop, 0.0, 0.0};
}

HomingCommand HomingController::update(uint32_t nowMs, bool rawSensor,
                                       double currentPosMm, bool isMoving) {
    const bool sensor = active(rawSensor);
    const double dir = cfg_.direction >= 0 ? 1.0 : -1.0;

    switch (state_) {
        case HomingState::Idle:
        case HomingState::Ready:
        case HomingState::Fault:
            return {MoveKind::Stop, 0.0, 0.0};

        case HomingState::CheckSensor:
            startPosMm_ = currentPosMm;
            if (sensor) return fail(HomingFault::SensorActiveAtStart);
            state_ = HomingState::SeekFast;
            return {MoveKind::MoveVelocity, cfg_.fastSpeedMmS * dir, 0.0};

        case HomingState::SeekFast: {
            if (timedOut(nowMs)) return fail(HomingFault::Timeout);
            if (std::fabs(currentPosMm - startPosMm_) > cfg_.maxSearchMm)
                return fail(HomingFault::MaxDistanceExceeded);
            if (sensor) {
                // Brake to a standstill BEFORE commanding the reverse move: a
                // reversing target issued while still moving can be ignored by
                // the hardware step engine.
                triggerPosMm_ = currentPosMm;
                state_ = HomingState::BrakeFast;
                return {MoveKind::Stop, 0.0, 0.0};
            }
            return {MoveKind::MoveVelocity, cfg_.fastSpeedMmS * dir, 0.0};
        }

        case HomingState::BrakeFast: {
            if (timedOut(nowMs)) return fail(HomingFault::Timeout);
            if (!isMoving) {  // motor truly stopped (step engine reports idle)
                // Back off from the sensor trigger, not the (speed-dependent)
                // stop position.
                backoffTargetMm_ = triggerPosMm_ - dir * cfg_.backoffMm;
                state_ = HomingState::Backoff;
                return {MoveKind::MoveTo, 0.0, backoffTargetMm_};
            }
            return {MoveKind::Stop, 0.0, 0.0};
        }

        case HomingState::Backoff: {
            if (timedOut(nowMs)) return fail(HomingFault::Timeout);
            bool reached = std::fabs(currentPosMm - backoffTargetMm_) < 0.05;
            if (reached) {
                if (sensor) return fail(HomingFault::SensorNotReleased);
                state_ = HomingState::SeekSlow;
                return {MoveKind::MoveVelocity, cfg_.slowSpeedMmS * dir, 0.0};
            }
            return {MoveKind::MoveTo, 0.0, backoffTargetMm_};
        }

        case HomingState::SeekSlow: {
            if (timedOut(nowMs)) return fail(HomingFault::Timeout);
            if (std::fabs(currentPosMm - startPosMm_) > cfg_.maxSearchMm)
                return fail(HomingFault::SensorNeverReached);
            if (sensor) {
                // Record the SENSOR TRIGGER position now — this is the zero
                // reference. The later stop position depends on braking distance
                // and must not become the reference.
                sensorTriggerPosMm_ = currentPosMm;
                state_ = HomingState::BrakeSlow;
                return {MoveKind::Stop, 0.0, 0.0};
            }
            return {MoveKind::MoveVelocity, cfg_.slowSpeedMmS * dir, 0.0};
        }

        case HomingState::BrakeSlow: {
            if (timedOut(nowMs)) return fail(HomingFault::Timeout);
            if (!isMoving) {  // motor truly stopped
                state_ = HomingState::SetZero;
                return {MoveKind::Stop, 0.0, 0.0};
            }
            return {MoveKind::Stop, 0.0, 0.0};
        }

        case HomingState::SetZero:
            // Zero = the sensor trigger point (repeatable). Move to the resting
            // offset relative to that trigger (away from the sensor).
            offsetTargetMm_ = sensorTriggerPosMm_ - dir * cfg_.offsetMm;
            state_ = HomingState::MoveToOffset;
            return {MoveKind::MoveTo, 0.0, offsetTargetMm_};

        case HomingState::MoveToOffset: {
            if (timedOut(nowMs)) return fail(HomingFault::Timeout);
            if (std::fabs(currentPosMm - offsetTargetMm_) < 0.05) {
                state_ = HomingState::Ready;
                return {MoveKind::Stop, 0.0, 0.0};
            }
            return {MoveKind::MoveTo, 0.0, offsetTargetMm_};
        }
    }
    return {MoveKind::Stop, 0.0, 0.0};
}

}  // namespace gmb
