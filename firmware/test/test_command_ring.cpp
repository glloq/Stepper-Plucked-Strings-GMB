// P2.17: the command-result ring extracted from main.cpp — outcome tracking with a
// fixed-size, wrap-around history.
#include "TestFramework.h"
#include "../src/core/util/CommandResultRing.h"

using namespace gmb;

TEST(command_ring_tracks_and_updates_state) {
    CommandResultRing r;
    CHECK(std::string(r.stateStr(1)) == "unknown");  // never issued
    r.set(1, CommandResultRing::Queued);
    CHECK(std::string(r.stateStr(1)) == "queued");
    r.set(1, CommandResultRing::Succeeded);           // update in place
    CHECK(std::string(r.stateStr(1)) == "succeeded");
    r.set(2, CommandResultRing::Refused);
    CHECK(std::string(r.stateStr(2)) == "refused");
    CHECK(std::string(r.stateStr(1)) == "succeeded");  // 1 still tracked
    r.set(0, CommandResultRing::Succeeded);            // id 0 ignored
    CHECK(std::string(r.stateStr(0)) == "unknown");
}

TEST(command_ring_evicts_oldest_after_capacity) {
    CommandResultRing r;
    // Fill well past capacity (16); the earliest ids must age out to "unknown".
    for (uint32_t id = 1; id <= 20; ++id) r.set(id, CommandResultRing::Succeeded);
    CHECK(std::string(r.stateStr(1)) == "unknown");   // evicted
    CHECK(std::string(r.stateStr(4)) == "unknown");   // evicted
    CHECK(std::string(r.stateStr(5)) == "succeeded"); // within the last 16
    CHECK(std::string(r.stateStr(20)) == "succeeded"); // newest retained
}

// A purged command reads back "cancelled", not a "queued" ghost (audit 6): the
// client following an activation stops immediately after a panic/E-stop purge.
TEST(command_ring_cancelled_state) {
    CommandResultRing r;
    r.set(42, CommandResultRing::Queued);
    CHECK_EQ(std::string(r.stateStr(42)), std::string("queued"));
    r.set(42, CommandResultRing::Cancelled);
    CHECK_EQ(std::string(r.stateStr(42)), std::string("cancelled"));
}

// Audit 7: the full deferred-activation lifecycle. "running" while the swap is
// in flight, then exactly one terminal state — succeeded ONLY at the new
// profile's real Ready, failed when the swap aborted mid-flight.
TEST(command_ring_running_and_failed_states) {
    CommandResultRing r;
    r.set(9, CommandResultRing::Queued);
    r.set(9, CommandResultRing::Running);            // handler deferred the outcome
    CHECK_EQ(std::string(r.stateStr(9)), std::string("running"));
    r.set(9, CommandResultRing::Succeeded);          // Parking -> Ready
    CHECK_EQ(std::string(r.stateStr(9)), std::string("succeeded"));

    r.set(10, CommandResultRing::Running);
    r.set(10, CommandResultRing::Failed);            // park unconfirmed / arm failed
    CHECK_EQ(std::string(r.stateStr(10)), std::string("failed"));

    r.set(11, CommandResultRing::Running);
    r.set(11, CommandResultRing::Cancelled);         // hard stop killed the swap
    CHECK_EQ(std::string(r.stateStr(11)), std::string("cancelled"));
}
