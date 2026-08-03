// Time-based contact debouncer for HOME / LIMIT / E-stop inputs
// (spec §13.2 robustness). A raw reading must hold steady for a
// configured window before it is accepted, so a bounce or a short spike cannot
// trigger a false reference, a false LIMIT, or a spurious E-stop.
//
// Pure C++ (no Arduino) so it is unit-tested on the host. Overflow-safe: uses
// signed time differences, so it survives the ~71 min micros()/millis() wrap.
#pragma once

#include <cstdint>

namespace gmb {

class Debouncer {
public:
    // `stableMs` is how long a new level must persist before it is accepted.
    // `initial` is the assumed starting state.
    void configure(uint32_t stableMs, bool initial = false) {
        stableMs_ = stableMs;
        stable_ = initial;
        candidate_ = initial;
        candidateSinceMs_ = 0;
        seeded_ = false;
    }

    // Feed the raw reading; returns the current debounced state.
    bool update(uint32_t nowMs, bool raw) {
        if (!seeded_) {
            seeded_ = true;
            candidate_ = raw;
            stable_ = raw;
            candidateSinceMs_ = nowMs;
            return stable_;
        }
        if (raw != candidate_) {
            candidate_ = raw;
            candidateSinceMs_ = nowMs;
        } else if (candidate_ != stable_ &&
                   static_cast<int32_t>(nowMs - candidateSinceMs_) >=
                       static_cast<int32_t>(stableMs_)) {
            stable_ = candidate_;  // held long enough: accept it
        }
        return stable_;
    }

    bool state() const { return stable_; }

private:
    uint32_t stableMs_ = 5;
    bool stable_ = false;
    bool candidate_ = false;
    uint32_t candidateSinceMs_ = 0;
    bool seeded_ = false;
};

}  // namespace gmb
