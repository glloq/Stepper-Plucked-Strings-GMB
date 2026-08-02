#include "MotionPlanner.h"

#include <cmath>

namespace gmb {

static double sgn(double v) { return (v > 0.0) - (v < 0.0); }

void MotionPlanner::update(double dt) {
    if (dt <= 0.0) return;

    if (velocityMode_) {
        // Ramp the velocity toward the cruise speed at maxAccel.
        double dv = cruise_ - velocity_;
        double step = maxAccel_ * dt;
        if (std::fabs(dv) <= step) velocity_ = cruise_;
        else velocity_ += sgn(dv) * step;
        if (velocity_ > maxSpeed_) velocity_ = maxSpeed_;
        if (velocity_ < -maxSpeed_) velocity_ = -maxSpeed_;
        position_ += velocity_ * dt;
        return;
    }

    double d = target_ - position_;
    if (std::fabs(d) < 1e-6 && std::fabs(velocity_) < 1e-6) {
        position_ = target_;
        velocity_ = 0.0;
        return;
    }

    const double s = sgn(d);
    // Distance needed to brake to zero from the current speed.
    const double stopDist = (velocity_ * velocity_) / (2.0 * maxAccel_);

    double accel;
    if (s * velocity_ < 0.0) {
        // Moving away from the target: brake toward zero first.
        accel = -sgn(velocity_) * maxAccel_;
    } else if (std::fabs(d) <= stopDist) {
        // Within braking distance: decelerate.
        accel = -s * maxAccel_;
    } else {
        // Cruise / accelerate toward the target.
        accel = s * maxAccel_;
    }

    velocity_ += accel * dt;
    if (velocity_ > maxSpeed_) velocity_ = maxSpeed_;
    if (velocity_ < -maxSpeed_) velocity_ = -maxSpeed_;

    double next = position_ + velocity_ * dt;
    // Snap to target if we crossed it (avoids overshoot chatter).
    if ((target_ - position_) * (target_ - next) <= 0.0) {
        position_ = target_;
        velocity_ = 0.0;
    } else {
        position_ = next;
    }
}

}  // namespace gmb
