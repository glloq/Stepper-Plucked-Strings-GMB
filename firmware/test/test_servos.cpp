#include "TestFramework.h"
#include "../src/core/configuration/Profile.h"
#include "../src/core/configuration/ProfileValidator.h"
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
