// MIDI velocity shaping (spec §18.1). Maps a 1..127 velocity to a
// 0..1 intensity through the configured curve; the mechanical layer uses the
// intensity for plectrum depth / attack.
#pragma once

#include <cmath>
#include <cstdint>

#include "../Types.h"

namespace gmb {

// Curve ids kept in sync with configuration/Profile.h VelocityCurve:
// 0 Linear, 1 Soft, 2 Hard, 3 Exponential, 4 Custom.
inline double applyVelocityCurve(int curve, uint8_t velocity) {
    double x = clampValue(static_cast<double>(velocity) / 127.0, 0.0, 1.0);
    switch (curve) {
        case 1: return x * x;                          // Soft (concave)
        case 2: return 1.0 - (1.0 - x) * (1.0 - x);    // Hard (convex)
        case 3: {                                      // Exponential
            const double k = 3.0;
            return (std::exp(k * x) - 1.0) / (std::exp(k) - 1.0);
        }
        case 0:                                        // Linear
        default:                                       // Custom -> linear default
            return x;
    }
}

}  // namespace gmb
