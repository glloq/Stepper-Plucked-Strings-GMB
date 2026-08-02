// STEP/DIR stepper control for 1..6 axes (cahier des charges §7.2, §12, §13).
//
// Each axis is driven by a trapezoidal MotionPlanner, so moves accelerate and
// decelerate (using maxSpeedMmS / maxAccelMmS2) instead of a fixed step rate,
// and homing can command a constant seek velocity. invertDirection is honoured
// through the mm->steps conversion. Stepping is ticked from loop() with no
// blocking waits during play (cahier des charges §16).
#pragma once

#include <cstdint>
#include <vector>

#include "../../core/motion/MotionPlanner.h"
#include "../../core/motion/StepperAxis.h"

#if defined(ARDUINO)
#include <Arduino.h>
#endif

namespace gmb {

struct AxisPins {
    int8_t step = -1;
    int8_t dir = -1;
    int8_t home = -1;
    int8_t limit = -1;
};

class StepperBank {
public:
    void begin(const std::vector<AxisConfig>& axes,
               const std::vector<AxisPins>& pins, int8_t enablePin);

    void enableDrivers(bool on);
    bool enabled() const { return enabled_; }

    // Position-mode move to an absolute target (mm, soft-limited).
    void moveToMm(size_t axis, double mm);
    // Same but without soft-limit clamping (used during homing, before the
    // coordinate system is anchored).
    void moveToMmRaw(size_t axis, double mm);
    // Velocity-mode cruise at a signed speed (mm/s), used by homing seeks.
    void setVelocityMm(size_t axis, double mmS);
    void stop(size_t axis);
    void stopAll();

    // Redefine the current physical position as `mm` (homing anchors home = 0).
    void setPositionReference(size_t axis, double mm);

    double positionMm(size_t axis) const;
    bool atTarget(size_t axis) const;
    bool homeActive(size_t axis) const;   // normalised active-low reading
    bool homeRawHigh(size_t axis) const;  // raw level (for polarity-aware homing)
    bool limitActive(size_t axis) const;

    // Step-pulse generation; call frequently from loop().
    void tick(uint32_t nowUs);

    size_t count() const { return axes_.size(); }

private:
    struct AxisRt {
        StepperAxis geom;
        MotionPlanner planner;
        AxisPins pins;
        long position = 0;   // stepped position (steps)
        bool homeActiveLow = true;
        AxisRt(const AxisConfig& c) : geom(c) {}
    };
    std::vector<AxisRt> axes_;
    int8_t enablePin_ = -1;
    bool enabled_ = false;
    uint32_t lastTickUs_ = 0;

    void writePin(int8_t pin, bool level);
    void doStep(AxisRt& a, bool forward);
};

}  // namespace gmb
