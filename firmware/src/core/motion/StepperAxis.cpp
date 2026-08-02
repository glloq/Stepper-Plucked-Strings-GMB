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

long StepperAxis::mmToSteps(double mm) const {
    double steps = mm * stepsPerMm();
    if (cfg_.invertDirection) steps = -steps;
    return std::lround(steps);
}

double StepperAxis::stepsToMm(long steps) const {
    double spm = stepsPerMm();
    if (spm == 0.0) return 0.0;
    double mm = static_cast<double>(steps) / spm;
    if (cfg_.invertDirection) mm = -mm;
    return mm;
}

double StepperAxis::fretPositionMm(int fret) const {
    if (fret < 0) fret = 0;
    if (fret < static_cast<int>(cfg_.calibratedFretMm.size())) {
        return cfg_.calibratedFretMm[fret];
    }
    return gmb::fretPositionMm(cfg_.scaleLengthMm, fret);
}

double StepperAxis::clampToLimits(double mm) const {
    return clampValue(mm, cfg_.minPositionMm, cfg_.maxPositionMm);
}

}  // namespace gmb
