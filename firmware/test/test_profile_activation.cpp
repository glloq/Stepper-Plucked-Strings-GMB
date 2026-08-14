// P2.17: the two-phase profile-swap state extracted from main.cpp — pending
// ownership + apply-when-the-mechanical-wait-elapses timing. Leaks are caught by ASan.
#include "TestFramework.h"
#include "../src/core/configuration/Profile.h"
#include "../src/core/configuration/ProfileActivation.h"

using namespace gmb;

static Profile named(const char* n) {
    return Profile::makeDefault(n, 4, {67, 60, 64, 69}, 12);
}

TEST(profile_activation_applies_after_the_wait) {
    ProfileActivation a;
    CHECK(!a.pending());
    a.begin(named("Uke"), /*applyAt=*/1000);
    CHECK(a.pending());

    std::string applied;
    // Before the wait: nothing happens, still pending.
    CHECK(!a.service(999, [&](const Profile& p) { applied = p.instrument.name; }));
    CHECK(a.pending());
    CHECK(applied.empty());

    // At the deadline: applied exactly once, then cleared.
    CHECK(a.service(1000, [&](const Profile& p) { applied = p.instrument.name; }));
    CHECK(applied == "Uke");
    CHECK(!a.pending());
    // A second service is a no-op.
    applied.clear();
    CHECK(!a.service(2000, [&](const Profile& p) { applied = p.instrument.name; }));
    CHECK(applied.empty());
}

TEST(profile_activation_begin_replaces_and_cancel_clears) {
    ProfileActivation a;
    a.begin(named("First"), 500);
    a.begin(named("Second"), 800);   // replaces the pending one (old copy freed)
    CHECK(a.pending());
    std::string applied;
    CHECK(a.service(800, [&](const Profile& p) { applied = p.instrument.name; }));
    CHECK(applied == "Second");

    // cancel() drops a pending swap without applying it.
    a.begin(named("Third"), 100);
    a.cancel();
    CHECK(!a.pending());
    bool ran = false;
    CHECK(!a.service(9999, [&](const Profile&) { ran = true; }));
    CHECK(!ran);
}

// Audit 7: a swap carries the id of the web command that requested it, so the
// owner can record the command's REAL outcome (Ready / failed / cancelled) at
// the end of the asynchronous activation instead of at "the procedure started".
TEST(profile_activation_tracks_the_owning_command) {
    ProfileActivation a;
    CHECK(a.commandId() == 0u);         // nothing pending -> no command
    a.begin(named("Uke"), 1000, 42u);
    CHECK(a.commandId() == 42u);

    // due() marks the pre-apply checkpoint (PCA health gate) exactly at the
    // mechanical deadline.
    CHECK(!a.due(999));
    CHECK(a.due(1000));

    // Applying clears the id with the pending profile.
    CHECK(a.service(1000, [](const Profile&) {}));
    CHECK(a.commandId() == 0u);
    CHECK(!a.due(5000));

    // cancel() clears it too (hard stop / superseded swap).
    a.begin(named("Uke"), 200, 7u);
    CHECK(a.commandId() == 7u);
    a.cancel();
    CHECK(a.commandId() == 0u);

    // begin() without an id (boot-time arming path) stays at 0.
    a.begin(named("Uke"), 300);
    CHECK(a.commandId() == 0u);
}
