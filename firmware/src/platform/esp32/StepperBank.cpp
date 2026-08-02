#include "StepperBank.h"

#include <algorithm>

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
        rt.stepsPerMm = rt.geom.stepsPerMm();
        // Minimum step interval from the configured max speed.
        double maxStepsPerSec = axes[i].maxSpeedMmS * rt.stepsPerMm;
        rt.stepIntervalUs =
            maxStepsPerSec > 0 ? static_cast<uint32_t>(1e6 / maxStepsPerSec) : 200;
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
#endif
    enableDrivers(false);
}

void StepperBank::enableDrivers(bool on) {
    enabled_ = on;
    // TMC2209 ENABLE is active-low.
    writePin(enablePin_, on ? false : true);
}

void StepperBank::moveToMm(size_t axis, double mm) {
    if (axis >= axes_.size()) return;
    AxisRt& a = axes_[axis];
    mm = a.geom.clampToLimits(mm);
    a.target = a.geom.mmToSteps(mm);
}

void StepperBank::stop(size_t axis) {
    if (axis < axes_.size()) axes_[axis].target = axes_[axis].position;
}

void StepperBank::stopAll() {
    for (auto& a : axes_) a.target = a.position;
}

double StepperBank::positionMm(size_t axis) const {
    if (axis >= axes_.size()) return 0.0;
    return axes_[axis].geom.stepsToMm(axes_[axis].position);
}

bool StepperBank::atTarget(size_t axis) const {
    return axis < axes_.size() && axes_[axis].position == axes_[axis].target;
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

void StepperBank::tick(uint32_t nowUs) {
    if (!enabled_) return;
    for (auto& a : axes_) {
        if (a.position == a.target) continue;
        if (nowUs - a.lastStepUs < a.stepIntervalUs) continue;
        a.lastStepUs = nowUs;
        bool forward = a.target > a.position;
        writePin(a.pins.dir, forward);
        // One step pulse.
        writePin(a.pins.step, true);
#if defined(ARDUINO)
        delayMicroseconds(3);  // >= driver minimum high time
#endif
        writePin(a.pins.step, false);
        a.position += forward ? 1 : -1;
    }
}

}  // namespace gmb
