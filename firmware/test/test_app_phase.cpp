// P2.17: the application phase -> wire-label mapping extracted from main.cpp
// (surfaced over /api/diagnostics and /api/status; the web UI depends on these
// exact strings, so they are pinned here).
#include "TestFramework.h"
#include "../src/core/app/AppPhase.h"

#include <string>

using namespace gmb;

TEST(app_phase_labels_are_stable) {
    CHECK(std::string(appPhaseName(AppPhase::ConfigSafe, false)) == "configSafe");
    CHECK(std::string(appPhaseName(AppPhase::Boot, false)) == "boot");
    CHECK(std::string(appPhaseName(AppPhase::Homing, false)) == "homing");
    CHECK(std::string(appPhaseName(AppPhase::Reconfiguring, false)) == "reconfiguring");
    CHECK(std::string(appPhaseName(AppPhase::Ready, false)) == "ready");
}

TEST(app_phase_degraded_only_refines_ready) {
    // degraded splits Ready into a distinct label...
    CHECK(std::string(appPhaseName(AppPhase::Ready, true)) == "readyDegraded");
    // ...but never changes any other phase (a fault before arming is still that phase).
    CHECK(std::string(appPhaseName(AppPhase::ConfigSafe, true)) == "configSafe");
    CHECK(std::string(appPhaseName(AppPhase::Boot, true)) == "boot");
    CHECK(std::string(appPhaseName(AppPhase::Homing, true)) == "homing");
    CHECK(std::string(appPhaseName(AppPhase::Reconfiguring, true)) == "reconfiguring");
}
