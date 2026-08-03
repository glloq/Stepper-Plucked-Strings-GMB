#include "StepperAxis.h"

#include <cmath>

#include "../Types.h"

namespace gmb {

double StepperAxis::stepsPerMm() const {
    const double stepsPerRev =
        static_cast<double>(cfg_.stepsPerRevolution) * cfg_.microsteps;
    switch (cfg_.transmission) {
        case Transmission::BeltGt2: {
            double travelPerRev = cfg_.pulleyTeeth * cfg_.beltPitchMm;
            if (travelPerRev <= 0.0) return cfg_.customStepsPerMm;
            return stepsPerRev / travelPerRev;
        }
        case Transmission::Screw:
            if (cfg_.leadPerRevolutionMm <= 0.0) return cfg_.customStepsPerMm;
            return stepsPerRev / cfg_.leadPerRevolutionMm;
        case Transmission::Custom:
        default:
            return cfg_.customStepsPerMm;
    }
}

// NOTE: direction inversion is applied in exactly ONE place — the driver DIR
// pin polarity (StepperBank::setDirectionPin). mm<->steps stays a pure, sign-
// preserving mechanical conversion so position and velocity modes agree.
long StepperAxis::mmToSteps(double mm) const {
    return std::lround(mm * stepsPerMm());
}

double StepperAxis::stepsToMm(long steps) const {
    double spm = stepsPerMm();
    if (spm == 0.0) return 0.0;
    return static_cast<double>(steps) / spm;
}

double StepperAxis::fretPositionMm(int fret) const {
    if (fret < 0) fret = 0;
    // Both the calibrated table and the theoretical spacing are measured from
    // the nut (fret 0 = 0). fretOffsetMm then places the nut relative to the
    // HOME endstop (the FDC), so the absolute axis target is offset + nut-value.
    // A single per-string offset shifts every fret at once, and calibration is
    // decoupled from the travel limits (minPositionMm), fixing audit P1-12.
    double nutRelative = (fret < static_cast<int>(cfg_.calibratedFretMm.size()))
        ? cfg_.calibratedFretMm[fret]
        : gmb::fretPositionMm(cfg_.scaleLengthMm, fret);
    return cfg_.fretOffsetMm + nutRelative;
}

double StepperAxis::clampToLimits(double mm) const {
    return clampValue(mm, cfg_.minPositionMm, cfg_.maxPositionMm);
}

}  // namespace gmb
