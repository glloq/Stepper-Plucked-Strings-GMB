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

// Up to four PCA boards addressable (0..3); board 4 is rejected.
TEST(pca_board_range) {
    Profile p = uke();
    ServoConfig ok;
    ok.enabled = true; ok.function = "strum"; ok.stringIndex = 0;
    ok.source = ServoSource::Pca; ok.pcaBoard = 3; ok.channel = 5;
    p.servos.push_back(ok);
    CHECK(ProfileValidator::isActivatable(p));

    p.servos.back().pcaBoard = 4;  // out of range
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

// A per-string strum servo counts as the individual striker (no pluck required).
TEST(per_string_strum_satisfies_individual_mode) {
    Profile p = uke();
    for (auto& s : p.servos)
        if (s.function == "pluck" && s.stringIndex == 0) s.function = "strum";
    CHECK(ProfileValidator::isActivatable(p));
}

// A strum lift with no striker to lift on its string is rejected.
TEST(strum_lift_without_striker_rejected) {
    Profile p = uke();
    // Shared-strum mode: per-string strikers are optional, so the only reason the
    // profile fails is the dangling strum lift.
    p.instrument.pluckMode = PluckMode::SharedStrum;
    ServoConfig shared;
    shared.enabled = true;
    shared.function = "sharedStrum";
    shared.stringIndex = -1;
    shared.source = ServoSource::Pca;
    shared.pcaBoard = 0;
    shared.channel = 12;
    p.servos.push_back(shared);
    // Drop string 0's pluck so it has no striker at all.
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
