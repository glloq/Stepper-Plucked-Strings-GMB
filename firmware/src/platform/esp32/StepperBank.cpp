#include "StepperBank.h"

#include <cmath>

namespace gmb {

#if defined(ARDUINO)

void StepperBank::begin(const std::vector<AxisConfig>& axes,
                        const std::vector<AxisPins>& pins, int8_t enablePin,
                        const std::vector<AxisEndstops>& endstops) {
    axes_.clear();
    steppers_.clear();
    enablePin_ = enablePin;
    attachFault_ = false;
    engine_.init();

    for (size_t i = 0; i < axes.size(); ++i) {
        AxisRt rt(axes[i]);
        rt.pins = i < pins.size() ? pins[i] : AxisPins{};
        rt.stepsPerMm = rt.geom.stepsPerMm();
        AxisEndstops es = i < endstops.size() ? endstops[i] : AxisEndstops{};
        rt.homeActiveHigh = es.homeActiveHigh;
        rt.limitActiveHigh = es.limitActiveHigh;
        // The settling time comes from the sensor technology (see EndstopType):
        // a contact bounces and must settle, an optical gate does not and its
        // filter would only lag the zero by slowSpeed × debounce.
        rt.homeDeb.configure(es.homeDebounceMs, false);
        rt.limitDeb.configure(es.limitDebounceMs, false);

        // A disabled axis is never attached and never faults the bank: it may
        // legitimately carry no STEP/DIR/HOME pins at all.
        if (!axes[i].enabled) {
            rt.attachFault = false;
            axes_.push_back(rt);
            steppers_.push_back(nullptr);
            continue;
        }

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
            rt.attachFault = true;  // no free RMT/MCPWM unit or STEP pin missing
            attachFault_ = true;    // at least one ENABLED axis failed to attach
        }
        axes_.push_back(rt);
        steppers_.push_back(s);

        // INPUT_PULLUP for BOTH sensor technologies, deliberately. A mechanical
        // switch to GND needs it. An open-collector / NPN optical module needs it
        // too. A push-pull optical module does not, but the ESP32's ~45 kΩ internal
        // pull-up loses to a driver sourcing milliamps, so it costs nothing there.
        // Making the pull mode configurable would add a knob whose only correct
        // setting is the one already chosen.
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

ActuatorResult StepperBank::moveToMm(size_t axis, double mm) {
    ActuatorResult g = axisWritable(axis);
    if (g != ActuatorResult::Ok) return g;
    double clamped = axes_[axis].geom.clampToLimits(mm);
    axes_[axis].cmdTargetMm = clamped;
    axes_[axis].hasTarget = true;
    steppers_[axis]->moveTo(axes_[axis].geom.mmToSteps(clamped));
    ++moveCount_;
    return ActuatorResult::Ok;
}

ActuatorResult StepperBank::moveToMmRaw(size_t axis, double mm) {
    ActuatorResult g = axisWritable(axis);
    if (g != ActuatorResult::Ok) return g;
    axes_[axis].cmdTargetMm = mm;
    axes_[axis].hasTarget = true;
    steppers_[axis]->moveTo(axes_[axis].geom.mmToSteps(mm));
    ++moveCount_;
    return ActuatorResult::Ok;
}

ActuatorResult StepperBank::setVelocityMm(size_t axis, double mmS) {
    ActuatorResult g = axisWritable(axis);
    if (g != ActuatorResult::Ok) return g;
    axes_[axis].hasTarget = false;  // velocity cruise has no position target
    double hz = std::fabs(mmS) * axes_[axis].stepsPerMm;
    steppers_[axis]->setSpeedInHz(static_cast<uint32_t>(hz > 1 ? hz : 1));
    if (mmS >= 0) steppers_[axis]->runForward();
    else steppers_[axis]->runBackward();
    return ActuatorResult::Ok;
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

void StepperBank::controlledStopAll() {
    // Decelerated stop on every axis, drivers left ENABLED so the carriages ramp
    // down under control instead of being abandoned mid-move. Never for an E-stop.
    for (auto* s : steppers_)
        if (s) s->stopMove();
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

bool StepperBank::reachedTarget(size_t axis) const {
    if (axis >= steppers_.size() || !steppers_[axis]) return true;
    if (steppers_[axis]->isRunning()) return false;
    if (!axes_[axis].hasTarget) return true;  // no position move commanded yet
    static constexpr double kPosToleranceMm = 0.5;
    return std::fabs(positionMm(axis) - axes_[axis].cmdTargetMm) <= kPosToleranceMm;
}

bool StepperBank::isRunning(size_t axis) const {
    return axis < steppers_.size() && steppers_[axis] && steppers_[axis]->isRunning();
}

void StepperBank::updateSensors(uint32_t nowMs) {
    for (auto& a : axes_) {
        if (a.pins.home >= 0) a.homeDeb.update(nowMs, digitalRead(a.pins.home) == HIGH);
        if (a.pins.limit >= 0) a.limitDeb.update(nowMs, digitalRead(a.pins.limit) == HIGH);
    }
}

bool StepperBank::homeActive(size_t axis) const {
    if (axis >= axes_.size() || axes_[axis].pins.home < 0) return false;
    bool high = axes_[axis].homeDeb.state();  // debounced
    return axes_[axis].homeActiveHigh ? high : !high;
}

bool StepperBank::homeRawHigh(size_t axis) const {
    if (axis >= axes_.size() || axes_[axis].pins.home < 0) return false;
    return axes_[axis].homeDeb.state();  // debounced raw HIGH
}

bool StepperBank::limitActive(size_t axis) const {
    if (axis >= axes_.size() || axes_[axis].pins.limit < 0) return false;
    bool high = axes_[axis].limitDeb.state();  // debounced, independent polarity
    return axes_[axis].limitActiveHigh ? high : !high;
}

#else  // ---- non-Arduino stub (kept analysable off-target) ----

void StepperBank::begin(const std::vector<AxisConfig>& axes,
                        const std::vector<AxisPins>& pins, int8_t enablePin,
                        const std::vector<AxisEndstops>&) {
    axes_.clear();
    enablePin_ = enablePin;
    for (size_t i = 0; i < axes.size(); ++i) {
        AxisRt rt(axes[i]);
        rt.pins = i < pins.size() ? pins[i] : AxisPins{};
        rt.stepsPerMm = rt.geom.stepsPerMm();
        axes_.push_back(rt);
    }
}
void StepperBank::updateSensors(uint32_t) {}
void StepperBank::enableDrivers(bool on) { enabled_ = on; }
ActuatorResult StepperBank::moveToMm(size_t axis, double mm) {
    ActuatorResult g = axisWritable(axis);
    if (g != ActuatorResult::Ok) return g;
    double clamped = axes_[axis].geom.clampToLimits(mm);
    axes_[axis].cmdTargetMm = clamped;
    axes_[axis].hasTarget = true;
    axes_[axis].position = axes_[axis].geom.mmToSteps(clamped);
    ++moveCount_;
    return ActuatorResult::Ok;
}
ActuatorResult StepperBank::moveToMmRaw(size_t axis, double mm) {
    ActuatorResult g = axisWritable(axis);
    if (g != ActuatorResult::Ok) return g;
    axes_[axis].cmdTargetMm = mm;
    axes_[axis].hasTarget = true;
    axes_[axis].position = axes_[axis].geom.mmToSteps(mm);
    ++moveCount_;
    return ActuatorResult::Ok;
}
ActuatorResult StepperBank::setVelocityMm(size_t axis, double) { return axisWritable(axis); }
void StepperBank::stop(size_t) {}
void StepperBank::emergencyStop(size_t) {}
void StepperBank::stopAll() {}
void StepperBank::controlledStopAll() {}
void StepperBank::setPositionReference(size_t axis, double mm) {
    if (axis < axes_.size()) axes_[axis].position = axes_[axis].geom.mmToSteps(mm);
}
double StepperBank::positionMm(size_t axis) const {
    return axis < axes_.size() ? axes_[axis].geom.stepsToMm(axes_[axis].position) : 0.0;
}
bool StepperBank::atTarget(size_t) const { return true; }
bool StepperBank::reachedTarget(size_t) const { return true; }
bool StepperBank::isRunning(size_t) const { return false; }
bool StepperBank::homeActive(size_t) const { return false; }
bool StepperBank::homeRawHigh(size_t) const { return false; }
bool StepperBank::limitActive(size_t) const { return false; }

#endif

// ---- portable, shared by both builds ---------------------------------------

ActuatorResult StepperBank::axisWritable(size_t axis) const {
    if (axis >= axes_.size()) return ActuatorResult::InvalidIndex;
    if (!axes_[axis].geom.config().enabled) return ActuatorResult::Disabled;
    if (axes_[axis].attachFault) return ActuatorResult::OutputFault;
#if defined(ARDUINO)
    // An axis with no step generator can accept nothing (belt and braces: an
    // enabled axis that failed to attach already sets attachFault above).
    if (axis >= steppers_.size() || !steppers_[axis]) return ActuatorResult::OutputFault;
#endif
    return ActuatorResult::Ok;
}

void StepperBank::hardStop() {
    // Motion-side twin of ServoBank::hardStop: force every axis to a dead stop
    // where it stands (no deceleration ramp, no wait for a move to finish), then
    // drop the shared driver ENABLE. Safety-ordered — the stop precedes the power
    // cut so a driver is never de-energised mid-ramp with the engine still pulsing.
    stopAll();
    enableDrivers(false);
}

uint32_t StepperBank::stopDurationMs() const {
    // Worst case across the enabled axes: decelerating from maxSpeed at maxAccel
    // takes v/a seconds. A 50 ms floor covers the engine's own reaction latency.
    double worst = 0.0;
    for (const auto& a : axes_) {
        const AxisConfig& c = a.geom.config();
        if (!c.enabled || c.maxAccelMmS2 <= 0.0) continue;
        double t = c.maxSpeedMmS / c.maxAccelMmS2;
        if (t > worst) worst = t;
    }
    if (worst <= 0.0) return 0;
    return static_cast<uint32_t>(worst * 1000.0) + 50u;
}

}  // namespace gmb
