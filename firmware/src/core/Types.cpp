#include "Types.h"

#include <cmath>

namespace gmb {

double fretPositionMm(double scaleLengthMm, int fret) {
    if (fret <= 0) return 0.0;
    return scaleLengthMm * (1.0 - std::pow(2.0, -static_cast<double>(fret) / 12.0));
}

}  // namespace gmb
