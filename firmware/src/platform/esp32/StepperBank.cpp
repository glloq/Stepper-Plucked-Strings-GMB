#include "StepperBank.h"

#include <cmath>

namespace gmb {

#if defined(ARDUINO)

void StepperBank::begin(const std::vector<AxisConfig>& axes,
                        const std::vector<AxisPins>& pins, int8_t enablePin) {
    axes_.clear();
    steppers_.clear();
    enablePin_ = enablePin;
    attachFault_ = false;
    engine_.init();

    for (size_t i = 0; i < axes.size(); ++i) {
        AxisRt rt(axes[i]);
        rt.pins = i < pins.size() ? pins[i] : AxisPins{};
        rt.stepsPerMm = rt.geom.stepsPerMm();
        axes_.push_back(rt);

        FastAccelStepper* s = nullptr;
        if (rt.pins.step >= 0) {
            s = engine_.stepperConnectToPin(rt.pins.step);
        }
        if (s != nullptr) {
            // Second arg = "dir HIGH counts position up"; flip it when inverted.
            if (rt.pins.dir >= 0) s->setDirectionPin(rt.pins.dir, !axes[i].invertDirection);
            s->setAutoEnable(false);
            double sps = axes[i].maxSpeedMmS * rt.stepsPerMm;
            double acc = axes[i].maxAccelMmS2 * rt.stepsPerMm;
            s->setSpeedInHz(static_cast<uint32_t>(sps > 1 ? sps : 1));
            s->setAcceleration(static_cast<uint32_t>(acc > 1 ? acc : 1));
        } else {
            attachFault_ = true;  // no free RMT/MCPWM unit or STEP pin missing
        }
        steppers_.push_back(s);

        if (rt.pins.home >= 0) pinMode(rt.pins.home, INPUT_PULLUP);
        if (rt.pins.limit >= 0) pinMode(rt.pins.limit, INPUT_PULLUP);
    }
    if (enablePin_ >= 0) pinMode(enablePin_, OUTPUT);
    enableDrivers(false);
}

void StepperBank::enableDrivers(bool on) {
    enabled_ = on;
    if (enablePin_ >= 0) digitalWrite(enablePin_, on ? LOW : HIGH);  // active-low
}

void StepperBank::moveToMm(size_t axis, double mm) {
    if (axis >= steppers_.size() || !steppers_[axis]) return;
    long steps = axes_[axis].geom.mmToSteps(axes_[axis].geom.clampToLimits(mm));
    steppers_[axis]->moveTo(steps);
}

void StepperBank::moveToMmRaw(size_t axis, double mm) {
    if (axis >= steppers_.size() || !steppers_[axis]) return;
    steppers_[axis]->moveTo(axes_[axis].geom.mmToSteps(mm));
}

void StepperBank::setVelocityMm(size_t axis, double mmS) {
    if (axis >= steppers_.size() || !steppers_[axis]) return;
    double hz = std::fabs(mmS) * axes_[axis].stepsPerMm;
    steppers_[axis]->setSpeedInHz(static_cast<uint32_t>(hz > 1 ? hz : 1));
    if (mmS >= 0) steppers_[axis]->runForward();
    else steppers_[axis]->runBackward();
}

void StepperBank::stop(size_t axis) {
    if (axis < steppers_.size() && steppers_[axis]) steppers_[axis]->stopMove();
}

void StepperBank::emergencyStop(size_t axis) {
    if (axis < steppers_.size() && steppers_[axis])
        steppers_[axis]->forceStopAndNewPosition(steppers_[axis]->getCurrentPosition());
}

void StepperBank::stopAll() {
    for (auto* s : steppers_)
        if (s) s->forceStopAndNewPosition(s->getCurrentPosition());
}

void StepperBank::setPositionReference(size_t axis, double mm) {
    if (axis >= steppers_.size() || !steppers_[axis]) return;
    steppers_[axis]->setCurrentPosition(axes_[axis].geom.mmToSteps(mm));
    // Restore the running speed after any homing-seek override.
    double sps = axes_[axis].geom.config().maxSpeedMmS * axes_[axis].stepsPerMm;
    steppers_[axis]->setSpeedInHz(static_cast<uint32_t>(sps > 1 ? sps : 1));
}

double StepperBank::positionMm(size_t axis) const {
    if (axis >= steppers_.size() || !steppers_[axis]) return 0.0;
    return axes_[axis].geom.stepsToMm(steppers_[axis]->getCurrentPosition());
}

bool StepperBank::atTarget(size_t axis) const {
    if (axis >= steppers_.size() || !steppers_[axis]) return true;
    return !steppers_[axis]->isRunning();
}

bool StepperBank::homeActive(size_t axis) const {
    if (axis >= axes_.size() || axes_[axis].pins.home < 0) return false;
    return digitalRead(axes_[axis].pins.home) == LOW;
}

bool StepperBank::homeRawHigh(size_t axis) const {
    if (axis >= axes_.size() || axes_[axis].pins.home < 0) return false;
    return digitalRead(axes_[axis].pins.home) == HIGH;
}

bool StepperBank::limitActive(size_t axis) const {
    if (axis >= axes_.size() || axes_[axis].pins.limit < 0) return false;
    return digitalRead(axes_[axis].pins.limit) == LOW;
}

#else  // ---- non-Arduino stub (kept analysable off-target) ----

void StepperBank::begin(const std::vector<AxisConfig>& axes,
                        const std::vector<AxisPins>& pins, int8_t enablePin) {
    axes_.clear();
    enablePin_ = enablePin;
    for (size_t i = 0; i < axes.size(); ++i) {
        AxisRt rt(axes[i]);
        rt.pins = i < pins.size() ? pins[i] : AxisPins{};
        rt.stepsPerMm = rt.geom.stepsPerMm();
        axes_.push_back(rt);
    }
}
void StepperBank::enableDrivers(bool on) { enabled_ = on; }
void StepperBank::moveToMm(size_t axis, double mm) {
    if (axis < axes_.size()) axes_[axis].position = axes_[axis].geom.mmToSteps(axes_[axis].geom.clampToLimits(mm));
}
void StepperBank::moveToMmRaw(size_t axis, double mm) {
    if (axis < axes_.size()) axes_[axis].position = axes_[axis].geom.mmToSteps(mm);
}
void StepperBank::setVelocityMm(size_t, double) {}
void StepperBank::stop(size_t) {}
void StepperBank::emergencyStop(size_t) {}
void StepperBank::stopAll() {}
void StepperBank::setPositionReference(size_t axis, double mm) {
    if (axis < axes_.size()) axes_[axis].position = axes_[axis].geom.mmToSteps(mm);
}
double StepperBank::positionMm(size_t axis) const {
    return axis < axes_.size() ? axes_[axis].geom.stepsToMm(axes_[axis].position) : 0.0;
}
bool StepperBank::atTarget(size_t) const { return true; }
bool StepperBank::homeActive(size_t) const { return false; }
bool StepperBank::homeRawHigh(size_t) const { return false; }
bool StepperBank::limitActive(size_t) const { return false; }

#endif

}  // namespace gmb
