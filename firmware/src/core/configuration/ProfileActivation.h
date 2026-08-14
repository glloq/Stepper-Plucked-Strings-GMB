// Two-phase profile-swap state (audit P2.17 — extracted from main.cpp).
//
// Activating a new profile is deferred: the OLD profile's fingers are first driven to
// rest and given time to lift (a controlled park) BEFORE the config is torn down, so a
// profile that removes/reassigns a finger servo can't leave a finger pressed while the
// new one arms. This owns the pending profile (RAII — no more scattered new/delete in
// main.cpp) and the time at which it may be applied.
//
// Pure C++17: no Arduino dependency, so the swap timing + ownership is unit-tested on
// the host (and leaks are caught by ASan).
#pragma once

#include <cstdint>

#include "Profile.h"

namespace gmb {

class ProfileActivation {
public:
    ProfileActivation() = default;
    ProfileActivation(const ProfileActivation&) = delete;
    ProfileActivation& operator=(const ProfileActivation&) = delete;
    ~ProfileActivation() { delete pending_; }

    // Begin (or replace) a pending swap: keep a copy of `p`, to be applied at
    // applyAtMs. `commandId` ties the swap back to the web command that requested
    // it (audit 7): the owner marks that command's FINAL outcome only when the
    // activation really completes (Ready), fails, or is cancelled — 0 = no command.
    void begin(const Profile& p, uint32_t applyAtMs, uint32_t commandId = 0) {
        delete pending_;
        pending_ = new Profile(p);
        applyAtMs_ = applyAtMs;
        commandId_ = commandId;
    }

    bool pending() const { return pending_ != nullptr; }

    // The web command awaiting this swap's outcome (0 when none / not pending).
    uint32_t commandId() const { return pending_ ? commandId_ : 0; }

    // True when a swap is pending AND its mechanical wait has elapsed — the owner
    // can run its pre-apply checks (PCA health) at exactly this boundary.
    bool due(uint32_t nowMs) const {
        return pending_ && static_cast<int32_t>(nowMs - applyAtMs_) >= 0;
    }

    // If a swap is pending AND its mechanical wait has elapsed, hand the pending
    // profile to `apply` (by const ref) and clear it. Returns true iff a swap was
    // applied this call. No-op (false) otherwise.
    template <typename Apply>
    bool service(uint32_t nowMs, Apply apply) {
        if (!due(nowMs)) return false;
        apply(static_cast<const Profile&>(*pending_));
        delete pending_;
        pending_ = nullptr;
        commandId_ = 0;
        return true;
    }

    // Drop any pending swap without applying it (panic / hard stop).
    void cancel() {
        delete pending_;
        pending_ = nullptr;
        commandId_ = 0;
    }

private:
    Profile* pending_ = nullptr;
    uint32_t applyAtMs_ = 0;
    uint32_t commandId_ = 0;
};

}  // namespace gmb
