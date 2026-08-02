// Trapezoidal motion profile (cahier des charges §12: vitesse et accélération
// maximales). Pure C++ so the acceleration logic is unit-tested on the host; the
// ESP32 StepperBank advances one planner per axis and steps toward its position,
// which gives smooth accel/decel instead of a fixed step rate.
#pragma once

namespace gmb {

class MotionPlanner {
public:
    void configure(double maxSpeed, double maxAccel) {
        maxSpeed_ = maxSpeed > 0 ? maxSpeed : 1.0;
        maxAccel_ = maxAccel > 0 ? maxAccel : 1.0;
    }
    void reset(double position) {
        position_ = position;
        target_ = position;
        velocity_ = 0.0;
        velocityMode_ = false;
    }

    // Position mode: drive to an absolute target with accel/decel.
    void moveTo(double target) {
        target_ = target;
        velocityMode_ = false;
    }
    // Velocity mode: cruise at a signed speed (used during homing seeks).
    void setVelocity(double speed) {
        cruise_ = speed;
        velocityMode_ = true;
    }
    void stop() {
        velocityMode_ = false;
        target_ = position_;
        velocity_ = 0.0;
    }

    // Advance by dt seconds.
    void update(double dt);

    double position() const { return position_; }
    double velocity() const { return velocity_; }
    bool atTarget() const {
        return !velocityMode_ && position_ == target_ && velocity_ == 0.0;
    }

private:
    double maxSpeed_ = 200.0;
    double maxAccel_ = 2000.0;
    double position_ = 0.0;
    double target_ = 0.0;
    double velocity_ = 0.0;
    double cruise_ = 0.0;
    bool velocityMode_ = false;
};

}  // namespace gmb
