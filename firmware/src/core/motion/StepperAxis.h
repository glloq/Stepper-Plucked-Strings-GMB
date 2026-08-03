// Stepper axis geometry and unit conversion (spec 12).
//
// The firmware works in millimetres and converts to motor steps through a
// transmission-dependent steps/mm factor, so the mechanical type is abstracted
// away from the rest of the motion code.
#pragma once

#include <cstdint>
#include <vector>

namespace gmb {

enum class Transmission : uint8_t { BeltGt2 = 0, Screw = 1, Custom = 2 };

struct AxisConfig {
    bool enabled = true;
    Transmission transmission = Transmission::BeltGt2;

    // Motor / driver.
    uint16_t stepsPerRevolution = 200;  // 1.8° motor
    uint16_t microsteps = 16;

    // Belt parameters.
    uint16_t pulleyTeeth = 20;
    double beltPitchMm = 2.0;  // GT2

    // Screw parameter.
    double leadPerRevolutionMm = 8.0;

    // Custom override (used when transmission == Custom).
    double customStepsPerMm = 80.0;

    bool invertDirection = false;

    // Travel limits (mm from home reference).
    double minPositionMm = 0.0;
    double maxPositionMm = 400.0;

    // Motion profile.
    double maxSpeedMmS = 200.0;
    double maxAccelMmS2 = 2000.0;

    // Note geometry.
    uint8_t openNote = 40;
    uint8_t maxFret = 12;
    double scaleLengthMm = 330.0;  // vibrating length

    // Optional calibrated fret positions (index = fret). Overrides theory.
    std::vector<double> calibratedFretMm;
};

class StepperAxis {
public:
    explicit StepperAxis(const AxisConfig& cfg) : cfg_(cfg) {}

    const AxisConfig& config() const { return cfg_; }
    void setConfig(const AxisConfig& cfg) { cfg_ = cfg; }

    // Assisted steps/mm computation (spec 12.1).
    double stepsPerMm() const;

    long mmToSteps(double mm) const;
    double stepsToMm(long steps) const;

    // Theoretical or calibrated fret position (spec 14.2 / 14.3).
    double fretPositionMm(int fret) const;

    // Clamp a target to the configured soft limits.
    double clampToLimits(double mm) const;

    // Current commanded position (mm). Updated by moveTo/setPosition.
    double positionMm() const { return positionMm_; }
    void setPositionMm(double mm) { positionMm_ = mm; }

private:
    AxisConfig cfg_;
    double positionMm_ = 0.0;
};

}  // namespace gmb
