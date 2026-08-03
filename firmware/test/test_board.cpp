#include "TestFramework.h"
#include "../src/core/board/BoardProfile.h"
#include "../src/core/board/PinManager.h"

using namespace gmb;

// Acceptance criteria 3, 4, 5 (spec 25).

TEST(board_reserved_pins_are_red) {
    BoardProfile b = makeEsp32S3DevKitC1();
    for (int8_t g : {0, 3, 19, 20, 43, 44, 45, 46, 48}) {
        const PinCapability* p = b.find(g);
        CHECK(p != nullptr);
        CHECK(p->reserved);
        CHECK(p->preference == PinPreference::Reserved);
    }
    // USB pins flagged.
    CHECK(b.find(19)->usb);
    CHECK(b.find(20)->usb);
}

TEST(board_step_candidates_exclude_reserved) {
    BoardProfile b = makeEsp32S3DevKitC1();
    auto cands = b.candidatesFor(SignalKind::Step);
    CHECK(!cands.empty());
    for (const PinCapability* p : cands) {
        CHECK(!p->reserved);
        CHECK(p->highSpeedOutput);
        CHECK(p->gpio != 19 && p->gpio != 20);
    }
}

TEST(auto_assign_matches_recommended_table) {
    BoardProfile b = makeEsp32S3DevKitC1();
    PinManager pm(b);
    PinRequest req;
    req.stringCount = 6;
    CHECK(pm.autoAssign(req));
    // Recommended table (spec 11.5).
    CHECK_EQ(pm.gpioOf("STEP1"), 4);
    CHECK_EQ(pm.gpioOf("STEP6"), 16);
    CHECK_EQ(pm.gpioOf("DIR1"), 17);
    CHECK_EQ(pm.gpioOf("HOME1"), 12);
    CHECK_EQ(pm.gpioOf("SDA"), 40);
    CHECK_EQ(pm.gpioOf("SCL"), 41);
    CHECK_EQ(pm.gpioOf("ENABLE"), 42);
    CHECK_EQ(pm.gpioOf("SERVO_OE"), 47);
    // A clean auto-assignment must validate.
    CHECK(pm.validate(true).empty());
}

TEST(duplicate_pin_is_rejected) {
    BoardProfile b = makeEsp32S3DevKitC1();
    PinManager pm(b);
    pm.assign("STEP1", SignalKind::Step, 4);
    pm.assign("STEP2", SignalKind::Step, 4);  // conflict
    auto errs = pm.validate(true);
    CHECK(!errs.empty());
    bool foundConflict = false;
    for (auto& e : errs)
        if (!e.conflictWith.empty()) foundConflict = true;
    CHECK(foundConflict);
}

TEST(usb_pin_rejected_when_reserved) {
    BoardProfile b = makeEsp32S3DevKitC1();
    PinManager pm(b);
    pm.assign("STEP1", SignalKind::Step, 19);  // USB pin
    CHECK(!pm.validate(true).empty());
}

TEST(step_on_input_incapable_pin_rejected) {
    BoardProfile b = makeEsp32S3DevKitC1();
    PinManager pm(b);
    // GPIO26 is a reserved flash pin — not a valid STEP output.
    pm.assign("STEP1", SignalKind::Step, 26);
    CHECK(!pm.validate(true).empty());
}
