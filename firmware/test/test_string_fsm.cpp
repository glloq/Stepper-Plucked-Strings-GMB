#include "TestFramework.h"
#include "../src/core/instrument/StringController.h"

using namespace gmb;

static StringController armed() {
    StringController c;
    c.enable();
    return c;
}

// Full happy path to a sounding note.
TEST(fsm_full_pluck_sequence) {
    StringController c = armed();
    uint32_t id = c.noteOn(5);  // fretted note
    CHECK(id != 0);
    CHECK(c.state() == StringState::ReleasingFinger);
    c.motionReached();
    CHECK(c.state() == StringState::PressingFinger);
    c.fingerPressed();
    CHECK(c.state() == StringState::Settling);
    c.settled();
    CHECK(c.state() == StringState::ReadyToPluck);
    CHECK(c.pluckArmed());
    CHECK(c.executePluck(id));
    CHECK(c.state() == StringState::Sustaining);
}

// Open string skips the finger press (cahier des charges 15.3).
TEST(fsm_open_string_skips_finger) {
    StringController c = armed();
    uint32_t id = c.noteOn(0);  // open
    CHECK(c.openString());
    c.motionReached();
    CHECK(c.state() == StringState::ReadyToPluck);
    CHECK(c.executePluck(id));
    CHECK(c.state() == StringState::Sustaining);
}

// Acceptance criteria 12 & 13: Note Off cancels a preparing attack; no delayed
// pluck runs after cancellation.
TEST(fsm_note_off_cancels_prepared_pluck) {
    StringController c = armed();
    uint32_t id = c.noteOn(5);
    c.motionReached();
    c.fingerPressed();
    c.settled();
    CHECK(c.pluckArmed());
    // Note Off arrives before the deferred pluck executes.
    c.noteOff();
    CHECK(!c.pluckArmed());
    // The old deferred pluck must be ignored (stale command id).
    CHECK(!c.executePluck(id));
    CHECK(c.state() != StringState::Sustaining);
}

// A stale command id from a replaced note must never trigger a pluck.
TEST(fsm_replaced_command_ignores_old_pluck) {
    StringController c = armed();
    uint32_t oldId = c.noteOn(5);
    c.motionReached();
    c.fingerPressed();
    c.settled();
    uint32_t newId = c.noteOn(7);  // replace with a new note
    CHECK(newId != oldId);
    CHECK(!c.executePluck(oldId));  // old id rejected
    CHECK(c.state() == StringState::ReleasingFinger);
}

// Panic drops any armed attack (cahier des charges 21.3).
TEST(fsm_panic_cancels_everything) {
    StringController c = armed();
    uint32_t id = c.noteOn(5);
    c.motionReached();
    c.fingerPressed();
    c.settled();
    c.panic();
    CHECK(!c.executePluck(id));
    CHECK(c.state() == StringState::Idle);
}
