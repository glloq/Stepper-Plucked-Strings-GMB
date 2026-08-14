// Application lifecycle phase (audit P2.17).
//
// This is the *application-level* view of where the runtime is — layered on top of
// the SafetyManager state machine, not a replacement for it. `Ready` means the
// instrument is armed and playing; the boot/homing/reconfiguring phases gate that.
// The enum lived inline in main.cpp; extracting it here makes the phase->label
// mapping (surfaced over /api/diagnostics and /api/status) host-testable and gives a
// future ApplicationRuntime a shared, portable home for its lifecycle type.
//
// Stepper note: where the servo build parks its servos to rest before arming, the
// stepper build must HOME every axis (seek the HOME sensor, anchor 0 mm) — the same
// slot in the lifecycle, so the phase is named `Homing` and keeps the existing
// `"homing"` wire label the web UI already consumes.
#pragma once

namespace gmb {

enum class AppPhase { ConfigSafe, Boot, Homing, Reconfiguring, Ready };

// Stable wire label for a phase. `degraded` only refines the Ready phase (one or more
// axes disabled by a fault but the instrument is still armed and playable). Kept
// byte-identical to the original inline switch so the web UI / diagnostics contract
// does not change.
inline const char* appPhaseName(AppPhase phase, bool degraded) {
    switch (phase) {
        case AppPhase::Ready:         return degraded ? "readyDegraded" : "ready";
        case AppPhase::Homing:        return "homing";
        case AppPhase::Reconfiguring: return "reconfiguring";
        case AppPhase::ConfigSafe:    return "configSafe";
        case AppPhase::Boot:          break;
    }
    return "boot";
}

}  // namespace gmb
