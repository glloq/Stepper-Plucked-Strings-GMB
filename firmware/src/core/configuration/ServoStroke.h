// Pure strum/pluck stroke shaping (unit-tested on the host).
//
// Given a servo's calibration and a normalised intensity (0..1, from MIDI
// velocity), compute the target pulse for a strike. Honours the guaranteed
// minimum strike depth and, on an alternate (up) stroke, the alternate active
// endpoint. Kept out of ServoBank (a platform class) so the maths is unit-tested
// natively without the Arduino runtime. `inverted` is NOT applied here — the
// bank mirrors within the pulse window at write time.
#pragma once

#include <cstdint>

#include "Profile.h"

namespace gmb {

inline uint16_t servoStrikeTargetUs(const ServoConfig& s, double intensity,
                                    bool upStroke) {
    if (intensity < 0.0) intensity = 0.0;
    if (intensity > 1.0) intensity = 1.0;

    // Active endpoint for this stroke. When alternation is on and this is an
    // up-stroke, use activeAltUs; activeAltUs == 0 means "mirror activeUs about
    // restUs" (a symmetric opposite stroke).
    int activeEnd = static_cast<int>(s.activeUs);
    if (s.alternateDirection && upStroke) {
        activeEnd = s.activeAltUs != 0
                        ? static_cast<int>(s.activeAltUs)
                        : (2 * static_cast<int>(s.restUs) - static_cast<int>(s.activeUs));
    }

    int span = activeEnd - static_cast<int>(s.restUs);
    double target = static_cast<double>(s.restUs) + intensity * span;

    // Guaranteed minimum depth toward the active side (grattage angle floor).
    if (s.minStrikeUs != 0) {
        if (span >= 0) {
            if (target < s.minStrikeUs) target = s.minStrikeUs;
        } else {
            if (target > s.minStrikeUs) target = s.minStrikeUs;
        }
    }

    // Clamp to the servo's mechanical pulse window.
    if (target < s.pulseMinUs) target = s.pulseMinUs;
    if (target > s.pulseMaxUs) target = s.pulseMaxUs;
    return static_cast<uint16_t>(target + 0.5);
}

}  // namespace gmb
