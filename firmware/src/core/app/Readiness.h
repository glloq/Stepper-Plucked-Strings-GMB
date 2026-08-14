// Runtime readiness / degraded-mode rule (audit P2.17, P1.5).
//
// A string is *ready* only if the profile enabled it AND no runtime fault has been
// raised on it (a servo write error, a PCA bus fault on its board, ...). The
// instrument is *degraded* when at least one string the profile enabled is no longer
// ready — it still plays, but advertises fewer strings. This rule decides the
// `readyDegraded` app phase and which strings appear in the GMB capability block, so
// it is pinned here and host-tested rather than living inline in main.cpp.
#pragma once

#include <cstddef>
#include <vector>

#include "../configuration/Profile.h"

namespace gmb {

struct Readiness {
    int originallyEnabled = 0;  // strings the profile marked enabled
    int ready = 0;              // of those, the ones with no runtime fault
    bool degraded = false;      // ready < originallyEnabled: an enabled string dropped out
};

// Disables, in `rp`, every string that is not ready (profile-disabled OR runtime-
// faulted), leaving `rp` as the ready-only view a capability rebuild should publish,
// and reports the readiness counts. `faulted[i]` marks a runtime fault on string i;
// `faulted` may be shorter than `rp.strings` (a missing entry means "not faulted").
// Pass `rp` as a copy of the live profile — the live profile must not be mutated.
inline Readiness applyRuntimeFaults(Profile& rp, const std::vector<bool>& faulted) {
    Readiness r;
    for (std::size_t i = 0; i < rp.strings.size(); ++i) {
        if (rp.strings[i].enabled) ++r.originallyEnabled;
        const bool faulted_i = (i < faulted.size() && faulted[i]);
        if (!rp.strings[i].enabled || faulted_i)
            rp.strings[i].enabled = false;
        else
            ++r.ready;
    }
    r.degraded = r.ready < r.originallyEnabled;
    return r;
}

}  // namespace gmb
