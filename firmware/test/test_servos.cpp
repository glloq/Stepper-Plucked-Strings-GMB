#include "TestFramework.h"
#include "../src/core/configuration/Profile.h"
#include "../src/core/configuration/ProfileValidator.h"
#include "../src/core/configuration/ServoStroke.h"
#include "../src/core/motion/StepperAxis.h"

using namespace gmb;

static Profile uke() {
    return Profile::makeDefault("Ukulele", 4, {67, 60, 64, 69}, 12);
}

// Default servos declare a source, a string and a role.
TEST(default_servos_have_source_and_string) {
    Profile p = uke();
    CHECK(!p.servos.empty());
    for (const auto& s : p.servos) {
        CHECK(s.source == ServoSource::Pca);
        CHECK(s.stringIndex >= 0);
        CHECK(s.function == "finger" || s.function == "pluck");
    }
    CHECK(ProfileValidator::isActivatable(p));
}

// A direct-GPIO servo on a free pin is valid (works without any PCA).
TEST(direct_gpio_servo_is_valid) {
    Profile p = uke();
    ServoConfig strum;
    strum.enabled = true;
    strum.function = "strum";
    strum.stringIndex = 0;
    strum.source = ServoSource::DirectGpio;
    strum.gpio = 2;  // recommended free pin on the DevKitC-1
    p.servos.push_back(strum);
    CHECK(ProfileValidator::isActivatable(p));
}

// A direct servo on a reserved pin is rejected.
TEST(direct_servo_on_reserved_pin_rejected) {
    Profile p = uke();
    ServoConfig s;
    s.enabled = true;
    s.function = "damper";
    s.stringIndex = 0;
    s.source = ServoSource::DirectGpio;
    s.gpio = 19;  // USB pin
    p.servos.push_back(s);
    CHECK(!ProfileValidator::isActivatable(p));
}

// A direct servo clashing with a stepper STEP pin is rejected.
TEST(direct_servo_conflicts_with_stepper_pin) {
    Profile p = uke();
    int8_t stepGpio = -1;
    for (const auto& a : p.pins)
        if (a.signal == "STEP1") stepGpio = a.gpio;
    CHECK(stepGpio >= 0);
    ServoConfig s;
    s.enabled = true;
    s.function = "strum";
    s.stringIndex = 1;
    s.source = ServoSource::DirectGpio;
    s.gpio = stepGpio;
    p.servos.push_back(s);
    CHECK(!ProfileValidator::isActivatable(p));
}

// Two servos on the same PCA board+channel conflict.
TEST(duplicate_pca_channel_rejected) {
    Profile p = uke();
    ServoConfig s;
    s.enabled = true;
    s.function = "damper";
    s.stringIndex = 0;
    s.source = ServoSource::Pca;
    s.pcaBoard = 0;
    s.channel = 0;  // already used by finger of string 0
    p.servos.push_back(s);
    CHECK(!ProfileValidator::isActivatable(p));
}

// Up to eight PCA boards addressable per bus (0..7, i.e. 0x40..0x47); board 8 is
// rejected. Two hardware I2C controllers means 16 distinct boards in total.
TEST(pca_board_range) {
    Profile p = uke();
    ServoConfig ok;
    ok.enabled = true; ok.function = "strum"; ok.stringIndex = 0;
    ok.source = ServoSource::Pca; ok.pcaBoard = 7; ok.channel = 5;
    p.servos.push_back(ok);
    CHECK(ProfileValidator::isActivatable(p));

    p.servos.back().pcaBoard = 8;  // out of range (only 0x40..0x47 exist)
    CHECK(!ProfileValidator::isActivatable(p));
}

// The same board index + channel on the OTHER I2C bus is a DIFFERENT physical
// chip, so it must not be reported as a channel clash. A real clash on the same
// bus still is one, and bus 1 requires its own SDA2/SCL2 pins.
TEST(pca_channel_key_includes_the_i2c_bus) {
    Profile p = uke();
    ServoConfig a;
    a.enabled = true; a.function = "strum"; a.stringIndex = 0;
    a.source = ServoSource::Pca; a.pcaBoard = 1; a.channel = 5; a.i2cBus = 0;
    ServoConfig b = a;
    b.function = "damper"; b.i2cBus = 1;      // same board+channel, other bus
    p.servos.push_back(a);
    p.servos.push_back(b);
    // Bus 1 is in use, so its own I2C pins become mandatory.
    CHECK(!ProfileValidator::isActivatable(p));
    PinAssignment sda2; sda2.signal = "SDA2"; sda2.gpio = 38; sda2.kind = SignalKind::I2cSda;
    PinAssignment scl2; scl2.signal = "SCL2"; scl2.gpio = 39; scl2.kind = SignalKind::I2cScl;
    p.pins.push_back(sda2);
    p.pins.push_back(scl2);
    CHECK(ProfileValidator::isActivatable(p));

    p.servos.back().i2cBus = 0;  // now a REAL clash on one bus
    CHECK(!ProfileValidator::isActivatable(p));
}

// An out-of-range I2C bus is refused outright (only Wire and Wire1 exist).
TEST(pca_i2c_bus_must_be_0_or_1) {
    Profile p = uke();
    ServoConfig s;
    s.enabled = true; s.function = "strum"; s.stringIndex = 0;
    s.source = ServoSource::Pca; s.pcaBoard = 0; s.channel = 9; s.i2cBus = 2;
    p.servos.push_back(s);
    CHECK(!ProfileValidator::isActivatable(p));
}

// A plectrum mute position must sit inside the servo's calibrated pulse window,
// otherwise the "rest against the string" pose would be clamped somewhere else.
TEST(mute_pulse_must_be_inside_the_pulse_window) {
    Profile p = uke();
    ServoConfig s;
    s.enabled = true; s.function = "pluck"; s.stringIndex = 1;
    s.source = ServoSource::Pca; s.pcaBoard = 2; s.channel = 4;
    s.pulseMinUs = 600; s.pulseMaxUs = 2400;
    s.restUs = 1000; s.activeUs = 1800;
    s.muteUs = 0;                       // no mute position: always fine
    p.servos.push_back(s);
    CHECK(ProfileValidator::isActivatable(p));
    p.servos.back().muteUs = 1200;      // inside the window
    CHECK(ProfileValidator::isActivatable(p));
    p.servos.back().muteUs = 300;       // below pulseMinUs
    CHECK(!ProfileValidator::isActivatable(p));
    p.servos.back().muteUs = 2900;      // above pulseMaxUs
    CHECK(!ProfileValidator::isActivatable(p));
}

// The governor caps and the global timing fields are bounded: a mis-typed value
// would either stall notes for tens of seconds or claim more concurrent starts
// than a PCA9685 has channels.
TEST(power_and_timing_bounds_are_enforced) {
    Profile p = uke();
    CHECK(ProfileValidator::isActivatable(p));
    p.power.maxConcurrentPerBoard = 17;          // a PCA9685 has 16 channels
    CHECK(!ProfileValidator::isActivatable(p));
    p.power.maxConcurrentPerBoard = 0;           // 0 = no per-board limit
    CHECK(ProfileValidator::isActivatable(p));
    p.power.staggerMs = 2000;
    CHECK(!ProfileValidator::isActivatable(p));
    p.power.staggerMs = 8;
    p.midi.noteExecutionDelayMs = 60000;
    CHECK(!ProfileValidator::isActivatable(p));
    p.midi.noteExecutionDelayMs = 0;
    p.midi.fingerLeadMs = 9000;
    CHECK(!ProfileValidator::isActivatable(p));
    p.midi.fingerLeadMs = 0;
    p.pluck.minStrikePct = 150;                  // a percentage
    CHECK(!ProfileValidator::isActivatable(p));
    p.pluck.minStrikePct = 40;
    CHECK(ProfileValidator::isActivatable(p));
}

// Announced polyphony is 0 (automatic) or at most the physical string count.
TEST(polyphony_max_is_bounded_by_the_string_count) {
    Profile p = uke();
    CHECK(p.instrument.polyphonyMax == 0);       // automatic by default
    CHECK(ProfileValidator::isActivatable(p));
    p.instrument.polyphonyMax = 4;
    CHECK(ProfileValidator::isActivatable(p));
    p.instrument.polyphonyMax = 7;               // > kMaxStrings
    CHECK(!ProfileValidator::isActivatable(p));
}

// A per-string strum-lift servo paired with the string's pluck servo validates.
TEST(strum_lift_with_striker_is_valid) {
    Profile p = uke();
    ServoConfig lift;
    lift.enabled = true;
    lift.function = "strumLift";
    lift.stringIndex = 0;
    lift.source = ServoSource::Pca;
    lift.pcaBoard = 0;
    lift.channel = 12;  // free channel (finger 0..5, pluck 6..11)
    p.servos.push_back(lift);
    CHECK(ProfileValidator::isActivatable(p));
}

// A per-string strum servo counts as the string's striker (no pluck required).
TEST(per_string_strum_satisfies_striker) {
    Profile p = uke();
    for (auto& s : p.servos)
        if (s.function == "pluck" && s.stringIndex == 0) s.function = "strum";
    CHECK(ProfileValidator::isActivatable(p));
}

// Every enabled string needs its own striker — there is no shared strummer.
TEST(string_without_striker_rejected) {
    Profile p = uke();
    for (auto it = p.servos.begin(); it != p.servos.end();) {
        if (it->function == "pluck" && it->stringIndex == 0) it = p.servos.erase(it);
        else ++it;
    }
    CHECK(!ProfileValidator::isActivatable(p));
}

// A strum lift with no striker to lift on its string is rejected.
TEST(strum_lift_without_striker_rejected) {
    Profile p = uke();
    // Drop string 0's pluck so it has no striker, then give it a strum lift.
    for (auto it = p.servos.begin(); it != p.servos.end();) {
        if (it->function == "pluck" && it->stringIndex == 0) it = p.servos.erase(it);
        else ++it;
    }
    ServoConfig lift;
    lift.enabled = true;
    lift.function = "strumLift";
    lift.stringIndex = 0;
    lift.source = ServoSource::Pca;
    lift.pcaBoard = 0;
    lift.channel = 13;
    p.servos.push_back(lift);
    CHECK(!ProfileValidator::isActivatable(p));
}

// An absurd absolute pulse width (outside the safe servo window) is rejected.
TEST(servo_pulse_absolute_range_rejected) {
    Profile p = uke();
    p.servos[0].pulseMaxUs = 4000;  // > 3000 µs absolute ceiling
    CHECK(!ProfileValidator::isActivatable(p));
}

// A profile with no enabled string can never arm and is rejected.
TEST(zero_enabled_strings_rejected) {
    Profile p = uke();
    for (auto& s : p.strings) s.enabled = false;
    CHECK(!ProfileValidator::isActivatable(p));
}

// When the selector is disabled its CC numbers are unused, so colliding
// string/fret CCs must NOT fail the profile.
TEST(disabled_selector_ignores_cc_collision) {
    Profile p = uke();
    p.selector.enabled = false;
    p.selector.string.ccNumber = 20;
    p.selector.fret.ccNumber = 20;  // identical, but selection is off
    CHECK(ProfileValidator::isActivatable(p));
}

// --- Strum stroke shaping (servoStrikeTargetUs) ---------------------------

static ServoConfig strumServo() {
    ServoConfig s;
    s.function = "strum";
    s.pulseMinUs = 500;
    s.pulseMaxUs = 2500;
    s.restUs = 1000;
    s.activeUs = 1800;
    return s;
}

// Velocity scales the strike depth linearly between rest and active.
TEST(strike_depth_follows_velocity) {
    ServoConfig s = strumServo();
    CHECK_EQ((int)servoStrikeTargetUs(s, 0.0, false), 1000);   // rest
    CHECK_EQ((int)servoStrikeTargetUs(s, 1.0, false), 1800);   // active
    CHECK_EQ((int)servoStrikeTargetUs(s, 0.5, false), 1400);   // midpoint
}

// minStrikeUs guarantees a floor depth so soft notes still catch the string.
TEST(min_strike_depth_floor) {
    ServoConfig s = strumServo();
    s.minStrikeUs = 1300;
    CHECK_EQ((int)servoStrikeTargetUs(s, 0.0, false), 1300);   // floored up
    CHECK_EQ((int)servoStrikeTargetUs(s, 1.0, false), 1800);   // full still reaches active
}

// Alternate direction: the up-stroke uses activeAltUs when provided.
TEST(alternate_stroke_uses_alt_endpoint) {
    ServoConfig s = strumServo();
    s.alternateDirection = true;
    s.activeAltUs = 600;
    CHECK_EQ((int)servoStrikeTargetUs(s, 1.0, false), 1800);   // down-stroke
    CHECK_EQ((int)servoStrikeTargetUs(s, 1.0, true), 600);     // up-stroke
}

// activeAltUs == 0 mirrors the active pulse about rest for a symmetric up-stroke.
TEST(alternate_stroke_mirrors_when_alt_zero) {
    ServoConfig s = strumServo();
    s.alternateDirection = true;   // activeAltUs stays 0
    // mirror of 1800 about rest 1000 = 2*1000 - 1800 = 200, clamped to pulseMin 500.
    CHECK_EQ((int)servoStrikeTargetUs(s, 1.0, true), 500);
}

// An out-of-window alternate/min pulse is rejected by validation.
TEST(strum_alt_pulse_out_of_range_rejected) {
    Profile p = uke();
    ServoConfig strum = strumServo();
    strum.enabled = true;
    strum.stringIndex = 0;
    strum.source = ServoSource::Pca;
    strum.pcaBoard = 0;
    strum.channel = 12;
    strum.activeAltUs = 3000;  // > pulseMaxUs
    p.servos.push_back(strum);
    CHECK(!ProfileValidator::isActivatable(p));
}

// A valid strum with alternation + floor + custom stroke time validates.
TEST(strum_stroke_fields_valid) {
    Profile p = uke();
    ServoConfig strum = strumServo();
    strum.enabled = true;
    strum.stringIndex = 0;
    strum.source = ServoSource::Pca;
    strum.pcaBoard = 0;
    strum.channel = 12;
    strum.alternateDirection = true;
    strum.activeAltUs = 700;
    strum.minStrikeUs = 1200;
    strum.strokeMs = 40;
    strum.engageDelayMs = 15;
    p.servos.push_back(strum);
    CHECK(ProfileValidator::isActivatable(p));
}

// Adjustable per-fret positions: the calibrated table overrides theory and is
// what the web fret editor writes.
TEST(adjustable_fret_positions) {
    AxisConfig cfg;
    cfg.scaleLengthMm = 330.0;
    cfg.maxFret = 3;
    // User nudges fret 1 to a measured value.
    cfg.calibratedFretMm = {0.0, 19.5, gmb::fretPositionMm(330.0, 2),
                            gmb::fretPositionMm(330.0, 3)};
    StepperAxis axis(cfg);
    CHECK_NEAR(axis.fretPositionMm(1), 19.5, 1e-9);           // manual override
    CHECK_NEAR(axis.fretPositionMm(2), gmb::fretPositionMm(330.0, 2), 1e-9);
}

// The per-string fret offset (nut position from the FDC) shifts every fret; the
// theoretical spacing is measured from the nut.
TEST(fret_offset_shifts_all_frets) {
    AxisConfig cfg;
    cfg.scaleLengthMm = 330.0;
    cfg.maxFret = 3;
    cfg.fretOffsetMm = 25.0;
    StepperAxis axis(cfg);
    CHECK_NEAR(axis.fretPositionMm(0), 25.0, 1e-9);                                 // nut at the offset
    CHECK_NEAR(axis.fretPositionMm(1), 25.0 + gmb::fretPositionMm(330.0, 1), 1e-9); // + spacing
}

// The offset applies on top of a nut-relative calibrated table too.
TEST(fret_offset_applies_to_calibrated) {
    AxisConfig cfg;
    cfg.scaleLengthMm = 330.0;
    cfg.maxFret = 2;
    cfg.fretOffsetMm = 10.0;
    cfg.calibratedFretMm = {0.0, 19.5, 37.0};  // nut-relative
    StepperAxis axis(cfg);
    CHECK_NEAR(axis.fretPositionMm(0), 10.0, 1e-9);
    CHECK_NEAR(axis.fretPositionMm(1), 10.0 + 19.5, 1e-9);
}

// The travel-fit validator must fold in fretOffsetMm (absolute target), else a
// too-large offset would be silently clamped at play time (audit P1-8 regression).
TEST(fret_offset_beyond_travel_rejected) {
    Profile p = uke();  // scale 330, maxFret 12 -> lastFret 165; maxPositionMm 400
    p.strings[0].fretOffsetMm = 300.0;  // 300 + 165 = 465 > 400
    CHECK(!ProfileValidator::isActivatable(p));
}
TEST(fret_offset_within_travel_valid) {
    Profile p = uke();
    p.strings[0].fretOffsetMm = 20.0;   // 20 + 165 = 185 < 400
    CHECK(ProfileValidator::isActivatable(p));
}
TEST(negative_fret_offset_before_travel_rejected) {
    Profile p = uke();
    p.strings[0].fretOffsetMm = -10.0;  // fret 0 target below minPositionMm (0)
    CHECK(!ProfileValidator::isActivatable(p));
}
// A calibrated value is nut-relative, so the range check must add the offset.
TEST(calibrated_plus_offset_out_of_travel_rejected) {
    Profile p = uke();
    p.strings[0].fretOffsetMm = 350.0;
    p.strings[0].calibratedFretMm = {0.0, 60.0};  // absolute: 350, 410 > 400
    CHECK(!ProfileValidator::isActivatable(p));
}
