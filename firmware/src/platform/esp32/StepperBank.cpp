#include "StepperBank.h"

#include <cmath>

namespace gmb {

void StepperBank::writePin(int8_t pin, bool level) {
#if defined(ARDUINO)
    if (pin >= 0) digitalWrite(pin, level ? HIGH : LOW);
#else
    (void)pin;
    (void)level;
#endif
}

void StepperBank::begin(const std::vector<AxisConfig>& axes,
                        const std::vector<AxisPins>& pins, int8_t enablePin) {
    axes_.clear();
    enablePin_ = enablePin;
    for (size_t i = 0; i < axes.size(); ++i) {
        AxisRt rt(axes[i]);
        rt.pins = i < pins.size() ? pins[i] : AxisPins{};
        rt.planner.configure(axes[i].maxSpeedMmS, axes[i].maxAccelMmS2);
        rt.planner.reset(0.0);
        axes_.push_back(rt);
    }
#if defined(ARDUINO)
    for (auto& a : axes_) {
        if (a.pins.step >= 0) pinMode(a.pins.step, OUTPUT);
        if (a.pins.dir >= 0) pinMode(a.pins.dir, OUTPUT);
        if (a.pins.home >= 0) pinMode(a.pins.home, INPUT_PULLUP);
        if (a.pins.limit >= 0) pinMode(a.pins.limit, INPUT_PULLUP);
    }
    if (enablePin_ >= 0) pinMode(enablePin_, OUTPUT);
    lastTickUs_ = micros();
#endif
    enableDrivers(false);
}

void StepperBank::enableDrivers(bool on) {
    enabled_ = on;
    writePin(enablePin_, on ? false : true);  // TMC2209 ENABLE is active-low
}

void StepperBank::moveToMm(size_t axis, double mm) {
    if (axis >= axes_.size()) return;
    axes_[axis].planner.moveTo(axes_[axis].geom.clampToLimits(mm));
}

void StepperBank::moveToMmRaw(size_t axis, double mm) {
    if (axis < axes_.size()) axes_[axis].planner.moveTo(mm);
}

void StepperBank::setVelocityMm(size_t axis, double mmS) {
    if (axis < axes_.size()) axes_[axis].planner.setVelocity(mmS);
}

void StepperBank::stop(size_t axis) {
    if (axis < axes_.size()) axes_[axis].planner.stop();
}

void StepperBank::stopAll() {
    for (auto& a : axes_) a.planner.stop();
}

void StepperBank::setPositionReference(size_t axis, double mm) {
    if (axis >= axes_.size()) return;
    axes_[axis].position = axes_[axis].geom.mmToSteps(mm);
    axes_[axis].planner.reset(mm);
}

double StepperBank::positionMm(size_t axis) const {
    if (axis >= axes_.size()) return 0.0;
    return axes_[axis].geom.stepsToMm(axes_[axis].position);
}

bool StepperBank::atTarget(size_t axis) const {
    if (axis >= axes_.size()) return true;
    const AxisRt& a = axes_[axis];
    return a.planner.atTarget() &&
           a.position == a.geom.mmToSteps(a.planner.position());
}

bool StepperBank::homeActive(size_t axis) const {
#if defined(ARDUINO)
    if (axis >= axes_.size() || axes_[axis].pins.home < 0) return false;
    return digitalRead(axes_[axis].pins.home) == LOW;  // active-low sensor
#else
    (void)axis;
    return false;
#endif
}

bool StepperBank::homeRawHigh(size_t axis) const {
#if defined(ARDUINO)
    if (axis >= axes_.size() || axes_[axis].pins.home < 0) return false;
    return digitalRead(axes_[axis].pins.home) == HIGH;
#else
    (void)axis;
    return false;
#endif
}

bool StepperBank::limitActive(size_t axis) const {
#if defined(ARDUINO)
    if (axis >= axes_.size() || axes_[axis].pins.limit < 0) return false;
    return digitalRead(axes_[axis].pins.limit) == LOW;
#else
    (void)axis;
    return false;
#endif
}

void StepperBank::doStep(AxisRt& a, bool forward) {
    writePin(a.pins.dir, forward);
    writePin(a.pins.step, true);
#if defined(ARDUINO)
    delayMicroseconds(3);  // >= driver minimum STEP high time
#endif
    writePin(a.pins.step, false);
    a.position += forward ? 1 : -1;
}

void StepperBank::tick(uint32_t nowUs) {
    if (!enabled_) {
        lastTickUs_ = nowUs;
        return;
    }
    double dt = static_cast<double>(nowUs - lastTickUs_) * 1e-6;
    lastTickUs_ = nowUs;
    if (dt <= 0.0) return;
    if (dt > 0.02) dt = 0.02;  // clamp after a stall so we don't lurch

    for (auto& a : axes_) {
        a.planner.update(dt);
        long want = a.geom.mmToSteps(a.planner.position());
        // Emit up to a few steps toward the planner position this tick.
        int budget = 8;
        while (a.position != want && budget-- > 0) {
            bool forward = want > a.position;
            doStep(a, forward);
        }
    }
}

}  // namespace gmb
