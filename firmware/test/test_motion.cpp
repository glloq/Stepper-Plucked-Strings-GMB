#include "TestFramework.h"
#include "../src/core/Types.h"
#include "../src/core/motion/HomingController.h"
#include "../src/core/motion/StepperAxis.h"

using namespace gmb;

TEST(steps_per_mm_belt) {
    AxisConfig cfg;
    cfg.transmission = Transmission::BeltGt2;
    cfg.stepsPerRevolution = 200;
    cfg.microsteps = 16;
    cfg.pulleyTeeth = 20;
    cfg.beltPitchMm = 2.0;
    StepperAxis axis(cfg);
    // 200*16 / (20*2) = 3200/40 = 80 steps/mm.
    CHECK_NEAR(axis.stepsPerMm(), 80.0, 1e-9);
    CHECK_EQ(axis.mmToSteps(10.0), 800);
}

TEST(steps_per_mm_screw) {
    AxisConfig cfg;
    cfg.transmission = Transmission::Screw;
    cfg.stepsPerRevolution = 200;
    cfg.microsteps = 16;
    cfg.leadPerRevolutionMm = 8.0;
    StepperAxis axis(cfg);
    // 200*16 / 8 = 400 steps/mm.
    CHECK_NEAR(axis.stepsPerMm(), 400.0, 1e-9);
}

TEST(fret_position_theory) {
    AxisConfig cfg;
    cfg.scaleLengthMm = 650.0;  // guitar scale
    StepperAxis axis(cfg);
    // 12th fret is exactly half the scale length.
    CHECK_NEAR(axis.fretPositionMm(12), 325.0, 1e-6);
    CHECK_NEAR(axis.fretPositionMm(0), 0.0, 1e-9);
}

TEST(calibrated_fret_overrides_theory) {
    AxisConfig cfg;
    cfg.scaleLengthMm = 650.0;
    cfg.calibratedFretMm = {0.0, 36.0, 70.0};
    StepperAxis axis(cfg);
    CHECK_NEAR(axis.fretPositionMm(1), 36.0, 1e-9);  // calibrated wins
    CHECK_NEAR(axis.fretPositionMm(5), gmb::fretPositionMm(650.0, 5), 1e-9);  // theory
}

// Simulate a full homing run against a virtual axis (cahier des charges 13).
TEST(homing_reaches_ready) {
    HomingConfig hc;
    hc.direction = -1;         // travel toward decreasing position
    hc.fastSpeedMmS = 40.0;
    hc.slowSpeedMmS = 5.0;
    hc.backoffMm = 3.0;
    hc.offsetMm = 0.0;
    hc.sensorActiveHigh = true;
    HomingController h;
    h.configure(hc);

    double pos = 50.0;         // start away from home
    double vel = 0.0;          // virtual axis with deceleration
    bool moving = false;
    const double homePos = 0.0;
    const double dt = 0.001;
    uint32_t t = 0;
    h.start(t);

    for (int i = 0; i < 100000 && !h.ready() && !h.failed(); ++i) {
        t += 1;  // 1 ms tick
        bool sensor = pos <= homePos + 0.2;  // sensor active near home
        HomingCommand cmd = h.update(t, sensor, pos, moving);
        if (cmd.kind == MoveKind::MoveVelocity) {
            vel = cmd.velocityMmS;
            pos += vel * dt;
            moving = vel != 0.0;
        } else if (cmd.kind == MoveKind::MoveTo) {
            double d = cmd.targetMm - pos;
            if (std::fabs(d) < 0.1) { pos = cmd.targetMm; vel = 0.0; moving = false; }
            else { pos += d > 0 ? 0.1 : -0.1; moving = true; }
        } else {  // Stop: decelerate over a few ticks so isMoving stays true briefly
            if (std::fabs(vel) > 1e-9) {
                vel *= 0.4;
                if (std::fabs(vel) < 1.0) vel = 0.0;
                pos += vel * dt;
                moving = vel != 0.0;
            } else {
                moving = false;
            }
        }
    }
    CHECK(h.ready());
    CHECK(!h.failed());
}

TEST(homing_faults_if_sensor_never_reached) {
    HomingConfig hc;
    hc.direction = -1;
    hc.fastSpeedMmS = 40.0;
    hc.maxSearchMm = 20.0;   // short leash
    hc.timeoutMs = 100000;
    HomingController h;
    h.configure(hc);
    double pos = 100.0;
    uint32_t t = 0;
    h.start(t);
    for (int i = 0; i < 100000 && !h.ready() && !h.failed(); ++i) {
        t += 1;
        HomingCommand cmd = h.update(t, false /*never active*/, pos, true);
        if (cmd.kind == MoveKind::MoveVelocity) pos += cmd.velocityMmS * 0.001;
    }
    CHECK(h.failed());
    CHECK(h.fault() == HomingFault::MaxDistanceExceeded);
}

// Sensor stuck active at start (never clears) -> SensorNotReleased fault.
TEST(homing_faults_if_sensor_stuck_at_start) {
    HomingConfig hc;
    hc.direction = -1;
    hc.slowSpeedMmS = 5.0;
    hc.backoffMm = 3.0;
    hc.timeoutMs = 100000;
    HomingController h;
    h.configure(hc);
    double pos = 0.0;
    uint32_t t = 0;
    h.start(t);
    for (int i = 0; i < 100000 && !h.ready() && !h.failed(); ++i) {
        t += 1;
        HomingCommand cmd = h.update(t, true /*stuck active*/, pos, true);
        if (cmd.kind == MoveKind::MoveVelocity) pos += cmd.velocityMmS * 0.001;
    }
    CHECK(h.failed());
    CHECK(h.fault() == HomingFault::SensorNotReleased);
}

// Sensor active at start but clears after a short back-off -> homes to ready.
TEST(homing_releases_then_reaches_ready) {
    HomingConfig hc;
    hc.direction = -1;
    hc.fastSpeedMmS = 40.0;
    hc.slowSpeedMmS = 5.0;
    hc.backoffMm = 3.0;
    hc.offsetMm = 0.0;
    HomingController h;
    h.configure(hc);
    double pos = 0.0, vel = 0.0;
    bool moving = false;
    const double dt = 0.001;
    uint32_t t = 0;
    h.start(t);
    for (int i = 0; i < 200000 && !h.ready() && !h.failed(); ++i) {
        t += 1;
        // Sensor active whenever pos is within 0.2 mm of home (0). At start pos=0
        // so it's active; backing off in +dir clears it; re-approach re-triggers.
        bool sensor = pos <= 0.0 + 0.2 && pos >= -0.5;
        HomingCommand cmd = h.update(t, sensor, pos, moving);
        if (cmd.kind == MoveKind::MoveVelocity) {
            vel = cmd.velocityMmS; pos += vel * dt; moving = vel != 0.0;
        } else if (cmd.kind == MoveKind::MoveTo) {
            double d = cmd.targetMm - pos;
            if (std::fabs(d) < 0.1) { pos = cmd.targetMm; vel = 0; moving = false; }
            else { pos += d > 0 ? 0.1 : -0.1; moving = true; }
        } else {
            if (std::fabs(vel) > 1e-9) { vel *= 0.4; if (std::fabs(vel) < 1) vel = 0; pos += vel * dt; moving = vel != 0.0; }
            else moving = false;
        }
    }
    CHECK(h.ready());
    CHECK(!h.failed());
}
