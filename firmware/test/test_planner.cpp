#include "TestFramework.h"
#include "../src/core/board/PinManager.h"
#include "../src/core/midi/Velocity.h"
#include "../src/core/motion/MotionPlanner.h"

using namespace gmb;

// The planner reaches the target, never exceeds max speed, and ramps velocity
// up then back to zero (uses maxAccel — the previously-unused field).
TEST(planner_reaches_target_with_accel) {
    MotionPlanner p;
    p.configure(/*maxSpeed=*/200.0, /*maxAccel=*/2000.0);
    p.reset(0.0);
    p.moveTo(100.0);

    double peak = 0.0;
    bool decelObserved = false;
    double prevV = 0.0;
    const double dt = 0.001;
    for (int i = 0; i < 100000 && !p.atTarget(); ++i) {
        p.update(dt);
        double v = std::fabs(p.velocity());
        CHECK(v <= 200.0 + 1e-6);        // never exceeds max speed
        if (v > peak) peak = v;
        if (v < prevV - 1e-9) decelObserved = true;
        prevV = v;
    }
    CHECK(p.atTarget());
    CHECK_NEAR(p.position(), 100.0, 1e-6);
    CHECK(peak > 0.0);
    CHECK(decelObserved);                // it decelerated near the end
}

// A short move never reaches full speed but still stops exactly on target.
TEST(planner_short_move_stops_clean) {
    MotionPlanner p;
    p.configure(500.0, 5000.0);
    p.reset(10.0);
    p.moveTo(11.0);
    for (int i = 0; i < 100000 && !p.atTarget(); ++i) p.update(0.001);
    CHECK(p.atTarget());
    CHECK_NEAR(p.position(), 11.0, 1e-6);
}

// Velocity mode ramps to the cruise speed (used by homing seeks).
TEST(planner_velocity_mode) {
    MotionPlanner p;
    p.configure(40.0, 400.0);
    p.reset(50.0);
    p.setVelocity(-40.0);
    for (int i = 0; i < 1000; ++i) p.update(0.001);
    CHECK_NEAR(p.velocity(), -40.0, 1e-6);
    CHECK(p.position() < 50.0);          // moved in the negative direction
}

// Signal kind is inferred from the signal name (fixes lost kind on import).
TEST(signal_kind_from_name) {
    CHECK(signalKindFromName("STEP1") == SignalKind::Step);
    CHECK(signalKindFromName("DIR4") == SignalKind::Dir);
    CHECK(signalKindFromName("HOME2") == SignalKind::Home);
    CHECK(signalKindFromName("LIMIT3") == SignalKind::Limit);
    CHECK(signalKindFromName("ENABLE") == SignalKind::Enable);
    CHECK(signalKindFromName("SDA") == SignalKind::I2cSda);
    CHECK(signalKindFromName("SCL") == SignalKind::I2cScl);
    CHECK(signalKindFromName("SERVO_OE") == SignalKind::ServoOe);
    CHECK(signalKindFromName("MYSTERY") == SignalKind::Generic);
}

// Velocity curves are monotonic and bounded 0..1.
TEST(velocity_curves) {
    for (int curve = 0; curve <= 3; ++curve) {
        CHECK_NEAR(applyVelocityCurve(curve, 0), 0.0, 1e-9);
        CHECK_NEAR(applyVelocityCurve(curve, 127), 1.0, 1e-9);
        double prev = -1.0;
        for (int v = 0; v <= 127; ++v) {
            double y = applyVelocityCurve(curve, static_cast<uint8_t>(v));
            CHECK(y >= -1e-9 && y <= 1.0 + 1e-9);
            CHECK(y >= prev - 1e-9);  // monotonic non-decreasing
            prev = y;
        }
    }
}
