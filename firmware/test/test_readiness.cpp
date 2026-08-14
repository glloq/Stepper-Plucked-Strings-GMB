// P2.17 / P1.5: the runtime readiness + degraded rule extracted from main.cpp's
// rebuildRuntimeCapabilities(). Decides the readyDegraded phase and which strings
// the GMB capability block advertises, so every branch is pinned here.
#include "TestFramework.h"
#include "../src/core/app/Readiness.h"
#include "../src/core/configuration/Profile.h"

#include <vector>

using namespace gmb;

// A profile with `n` strings, each enabled per the flags given.
static Profile withStrings(std::vector<bool> enabled) {
    Profile p = Profile::makeDefault("t", static_cast<uint8_t>(enabled.size()),
                                     std::vector<uint8_t>(enabled.size(), 60), 12);
    p.strings.resize(enabled.size());
    for (size_t i = 0; i < enabled.size(); ++i) p.strings[i].enabled = enabled[i];
    return p;
}

TEST(readiness_all_enabled_no_fault_is_not_degraded) {
    Profile p = withStrings({true, true, true});
    Readiness r = applyRuntimeFaults(p, {false, false, false});
    CHECK(r.originallyEnabled == 3);
    CHECK(r.ready == 3);
    CHECK(!r.degraded);
    // No string was disabled by the pass.
    CHECK(p.strings[0].enabled && p.strings[1].enabled && p.strings[2].enabled);
}

TEST(readiness_a_fault_on_an_enabled_string_degrades_and_disables_it) {
    Profile p = withStrings({true, true, true});
    Readiness r = applyRuntimeFaults(p, {false, true, false});
    CHECK(r.originallyEnabled == 3);
    CHECK(r.ready == 2);
    CHECK(r.degraded);
    // The faulted string is dropped from the published (ready-only) view...
    CHECK(!p.strings[1].enabled);
    // ...the healthy ones stay.
    CHECK(p.strings[0].enabled && p.strings[2].enabled);
}

TEST(readiness_profile_disabled_string_is_not_degraded) {
    // A string the profile never enabled is not "dropped out" — not degraded.
    Profile p = withStrings({true, false, true});
    Readiness r = applyRuntimeFaults(p, {false, false, false});
    CHECK(r.originallyEnabled == 2);
    CHECK(r.ready == 2);
    CHECK(!r.degraded);
}

TEST(readiness_fault_on_already_disabled_string_changes_nothing) {
    Profile p = withStrings({true, false, true});
    Readiness r = applyRuntimeFaults(p, {false, true, false});  // fault on the disabled one
    CHECK(r.originallyEnabled == 2);
    CHECK(r.ready == 2);
    CHECK(!r.degraded);
}

TEST(readiness_faulted_vector_may_be_shorter_than_strings) {
    // A short faulted vector means "no fault" for the missing tail (no OOB read).
    Profile p = withStrings({true, true, true, true});
    Readiness r = applyRuntimeFaults(p, {false, true});  // only covers strings 0,1
    CHECK(r.originallyEnabled == 4);
    CHECK(r.ready == 3);          // string 1 faulted; 0,2,3 ready
    CHECK(r.degraded);
    CHECK(!p.strings[1].enabled);
    CHECK(p.strings[2].enabled && p.strings[3].enabled);
}

TEST(readiness_empty_profile_is_not_degraded) {
    Profile p = withStrings({});
    Readiness r = applyRuntimeFaults(p, {});
    CHECK(r.originallyEnabled == 0);
    CHECK(r.ready == 0);
    CHECK(!r.degraded);
}
