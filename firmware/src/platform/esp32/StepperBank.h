// STEP/DIR stepper control for 1..6 axes (cahier des charges §7.2, §12).
//
// Software step generation ticked from loop(); no blocking delays during play
// (cahier des charges §16). Positions are tracked in steps and exposed in mm via
// the core StepperAxis geometry.
#pragma once

#include <cstdint>
#include <vector>

#include "../../core/board/PinManager.h"
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
    // Wire up axes from the profile's geometry and the resolved pin map.
    void begin(const std::vector<AxisConfig>& axes,
               const std::vector<AxisPins>& pins, int8_t enablePin);

    void enableDrivers(bool on);
    bool enabled() const { return enabled_; }

    // Command a target position in mm (soft-limited).
    void moveToMm(size_t axis, double mm);
    void stop(size_t axis);
    void stopAll();

    double positionMm(size_t axis) const;
    bool atTarget(size_t axis) const;
    bool homeActive(size_t axis) const;

    // Called frequently from loop(); generates step pulses by timing.
    void tick(uint32_t nowUs);

    size_t count() const { return axes_.size(); }

private:
    struct AxisRt {
        StepperAxis geom;
        AxisPins pins;
        long position = 0;   // steps
        long target = 0;     // steps
        double stepsPerMm = 80.0;
        uint32_t lastStepUs = 0;
        uint32_t stepIntervalUs = 200;  // derived from maxSpeed
        AxisRt(const AxisConfig& c) : geom(c) {}
    };
    std::vector<AxisRt> axes_;
    int8_t enablePin_ = -1;
    bool enabled_ = false;

    void writePin(int8_t pin, bool level);
};

}  // namespace gmb
