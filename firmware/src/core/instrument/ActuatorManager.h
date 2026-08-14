// Actuator movement manager (audit P1.6).
//
// Sits between the playback scheduler and the ServoBank:
//
//     PlaybackScheduler -> ActuatorManager -> PowerGovernor -> ServoBank
//
// and decides whether a movement must WAIT for a power-governor start-permit. It
// encodes the one distinction the mission calls out: musical sound must never be
// throttled, but the current-hungry positioning moves can be spread to bound the
// PCA9685 in-rush.
//
//   • Deadline   movements (a pluck / strum STRIKE) must fire on the sonic beat —
//                they are ALWAYS permitted, never governed.
//   • Staggerable movements (a finger PRESS, a strum-lift ENGAGE) draw their peak
//                current at start and have slack, so they go through the governor's
//                global + per-board caps and staggerMs window.
//
// Pure C++17 wrapping the (already unit-tested) ServoActivationGovernor, so the
// deadline-vs-staggerable policy itself is host-testable.
#pragma once

#include <cstdint>

#include "ServoActivationGovernor.h"

namespace gmb {

enum class MoveClass : uint8_t {
    Staggerable,  // finger press / release / strum-lift engage — governed
    Deadline,     // pluck / strum strike — must hit the beat, never governed
};

class ActuatorManager {
public:
    // Configure the underlying governor: global cap (0 = none), per-PCA-board cap
    // (0 = none) and the stagger window (0 = throttling off).
    void configure(uint8_t maxConcurrent, uint8_t maxPerBoard, uint16_t staggerMs) {
        governor_.configure(maxConcurrent, maxPerBoard, staggerMs);
    }
    void reset() { governor_.reset(); }

    // Register a movement START of class `cls` on PCA board bucket `board` (0xFF = a
    // servo on no board, e.g. direct GPIO — only the global cap applies) and return
    // whether it may proceed NOW. EVERY scheduler movement is registered here, so the
    // manager accounts for the whole in-rush picture: deadline movements (sound) are
    // always permitted and counted; staggerable ones defer to the governor's caps.
    bool requestMove(MoveClass cls, uint32_t nowMs, uint8_t board = 0xFF) {
        if (cls == MoveClass::Deadline) { ++deadlineMoves_; return true; }  // never throttled
        if (governor_.requestStart(nowMs, board)) { ++staggerableGrants_; return true; }
        return false;  // deferred (the governor counts it as a throttle)
    }

    // Forecast (audit 4 P1.3): ms until one new STAGGERABLE start could be granted
    // at nowMs on `board`, given the governor's current window state. Pure query.
    uint32_t nextSlotDelayMs(uint32_t nowMs, uint8_t board = 0xFF) const {
        return governor_.nextSlotDelayMs(nowMs, board);
    }
    // Full batch forecast (audit 5): ms after nowMs until the LAST of `count`
    // staggerable starts (on boards[]) could begin, simulated from the governor's
    // current window state. Pure query.
    uint32_t forecastBatchDelayMs(uint32_t nowMs, const uint8_t* boards,
                                  size_t count) const {
        return governor_.forecastBatchDelayMs(nowMs, boards, count);
    }

    // Diagnostics (P2.19): the movement mix the manager has seen.
    uint32_t deadlineMoves() const { return deadlineMoves_; }       // sound strikes passed
    uint32_t staggerableGrants() const { return staggerableGrants_; }  // positioning starts granted
    uint32_t throttleCount() const { return governor_.throttleCount(); }  // positioning starts deferred

private:
    ServoActivationGovernor governor_;
    uint32_t deadlineMoves_ = 0;
    uint32_t staggerableGrants_ = 0;
};

}  // namespace gmb
