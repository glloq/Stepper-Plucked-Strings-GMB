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

// ---- audit P1.14 / P1.15: every announced board, and GPIO0 reserved on all ----

// A board is only "supported" if the firmware can validate against it. All four
// announced identifiers must resolve; anything else must not silently fall back.
TEST(all_announced_boards_resolve) {
    for (const char* id : {"esp32-s3-devkitc-1", "esp32-s3-devkitc-1-v1.1",
                           "esp32-wroom-32", "esp32-devkit-v1"}) {
        const BoardProfile* b = builtinBoardProfile(id);
        CHECK(b != nullptr);
        if (b) CHECK(b->identifier == std::string(id));
    }
    CHECK(builtinBoardProfile("not-a-board") == nullptr);
}

// GPIO0 is the BOOT button the firmware samples to force the Wi-Fi hotspot, so it
// must never be assignable to ANY signal on ANY board — otherwise the escape hatch
// (and the bootloader entry) fights whatever we drive on it.
TEST(gpio0_boot_button_reserved_on_all_boards) {
    const SignalKind kinds[] = {SignalKind::Step, SignalKind::Dir, SignalKind::Enable,
                                SignalKind::Home, SignalKind::Limit, SignalKind::Diag,
                                SignalKind::I2cSda, SignalKind::I2cScl,
                                SignalKind::ServoOe, SignalKind::Generic,
                                SignalKind::SafetyInput, SignalKind::UartRx};
    for (const char* id : {"esp32-s3-devkitc-1", "esp32-s3-devkitc-1-v1.1",
                           "esp32-wroom-32", "esp32-devkit-v1"}) {
        const BoardProfile* b = builtinBoardProfile(id);
        CHECK(b != nullptr);
        if (!b) continue;
        const PinCapability* p = b->find(0);
        CHECK(p != nullptr);
        if (p) CHECK(p->reserved);
        for (SignalKind k : kinds) {
            CHECK(!b->supports(0, k));
            // ...and it never turns up as an auto-assignment candidate either.
            for (const PinCapability* c : b->candidatesFor(k)) CHECK(c->gpio != 0);
        }
    }
}

// The two DevKitC-1 revisions differ ONLY in which GPIO carries the RGB LED: the
// LED pin is reserved on each, and free on the other.
TEST(s3_devkit_revisions_differ_only_by_the_led_pin) {
    const BoardProfile* v10 = builtinBoardProfile("esp32-s3-devkitc-1");
    const BoardProfile* v11 = builtinBoardProfile("esp32-s3-devkitc-1-v1.1");
    CHECK(v10 != nullptr && v11 != nullptr);
    if (!v10 || !v11) return;
    CHECK(v10->find(48)->reserved);    // v1.0: LED on 48
    CHECK(!v10->find(38)->reserved);
    CHECK(v11->find(38)->reserved);    // v1.1: LED moved to 38
    CHECK(!v11->find(48)->reserved);
    CHECK(v10->pins.size() == v11->pins.size());
}

// Classic-ESP32 input-only pins (34/35/36/39) can carry no OUTPUT signal — a STEP
// or DIR line there would simply never toggle.
TEST(classic_esp32_input_only_pins_carry_no_output_signal) {
    for (const char* id : {"esp32-wroom-32", "esp32-devkit-v1"}) {
        const BoardProfile* b = builtinBoardProfile(id);
        CHECK(b != nullptr);
        if (!b) continue;
        for (int8_t g : {34, 35, 36, 39}) {
            CHECK(!b->supports(g, SignalKind::Step));
            CHECK(!b->supports(g, SignalKind::Dir));
            CHECK(!b->supports(g, SignalKind::I2cSda));
            CHECK(!b->supports(g, SignalKind::ServoOe));
        }
    }
}

// The hardware E-stop needs an interrupt-capable input WITH an internal pull-up and
// must never sit on a strapping pin: the recommended normally-closed loop holds the
// pin LOW while the machine may run, including through a reset.
TEST(safety_input_refuses_strapping_and_pull_less_pins) {
    const BoardProfile* w = builtinBoardProfile("esp32-wroom-32");
    CHECK(w != nullptr);
    if (!w) return;
    CHECK(!w->supports(0, SignalKind::SafetyInput));   // BOOT / strapping
    CHECK(!w->supports(2, SignalKind::SafetyInput));   // strapping
    CHECK(!w->supports(12, SignalKind::SafetyInput));  // MTDI strapping
    CHECK(!w->supports(15, SignalKind::SafetyInput));  // MTDO strapping
    CHECK(!w->supports(34, SignalKind::SafetyInput));  // input-only, no pull-up
    CHECK(w->supports(13, SignalKind::SafetyInput));   // an ordinary GPIO is fine
    for (const PinCapability* c : w->candidatesFor(SignalKind::SafetyInput))
        CHECK(!c->strapping && c->internalPullUp);
}

// DIN MIDI in (`MIDI_RX`). The UART matrix routes RX to any readable pin, so the
// only real constraints are "readable" and "not a strapping pin" — a powered MIDI
// sender holds the line at whatever it likes across a reset, which is exactly what
// a strapping pin must not see. Deliberately WIDER than SafetyInput: an input-only
// pin with no pull-up is a perfectly good UART RX, and refusing those would throw
// away four of the classic ESP32's few remaining free inputs.
TEST(uart_rx_accepts_input_only_pins_but_refuses_strapping) {
    const BoardProfile* w = builtinBoardProfile("esp32-wroom-32");
    CHECK(w != nullptr);
    if (!w) return;
    CHECK(!w->supports(0, SignalKind::UartRx));    // BOOT / strapping
    CHECK(!w->supports(2, SignalKind::UartRx));    // strapping
    CHECK(!w->supports(12, SignalKind::UartRx));   // MTDI strapping
    CHECK(!w->supports(15, SignalKind::UartRx));   // MTDO strapping
    CHECK(w->supports(34, SignalKind::UartRx));    // input-only: fine for RX
    CHECK(w->supports(13, SignalKind::UartRx));
    for (const PinCapability* c : w->candidatesFor(SignalKind::UartRx))
        CHECK(c->input && !c->strapping);
}

// PinManager must map the `MIDI_RX` signal name onto the UART kind, or the pin
// validator would fall through to Generic ("any output") and cheerfully accept an
// output-only pin for an input the firmware then reads forever as idle.
TEST(pin_manager_maps_midi_rx_to_uart_rx) {
    CHECK(signalKindFromName("MIDI_RX") == SignalKind::UartRx);
    CHECK(signalKindFromName("ESTOP") == SignalKind::SafetyInput);
}
