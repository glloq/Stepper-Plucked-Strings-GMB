#include "StringController.h"

namespace gmb {

uint32_t StringController::noteOn(int fret) {
    if (state_ == StringState::Disabled || state_ == StringState::Fault) {
        return 0;
    }
    commandId_ = nextId_++;
    targetFret_ = fret;
    openString_ = (fret == 0);
    pluckArmed_ = false;
    armOnSettle_ = true;  // direct play: arm the pluck as soon as the string is ready
    // The finger always lifts before moving (also the resting state for an open
    // string).
    state_ = StringState::ReleasingFinger;
    return commandId_;
}

uint32_t StringController::prepareNote(int fret) {
    if (state_ == StringState::Disabled || state_ == StringState::Fault) {
        return 0;
    }
    commandId_ = nextId_++;
    targetFret_ = fret;
    openString_ = (fret == 0);
    pluckArmed_ = false;
    armOnSettle_ = false;  // anticipation: move + press, but hold the pluck disarmed
    state_ = StringState::ReleasingFinger;
    return commandId_;
}

bool StringController::trigger(uint32_t id) {
    // Only the matching (still-current) prepared command may be triggered.
    if (id != commandId_) return false;
    if (state_ == StringState::Disabled || state_ == StringState::Fault) return false;
    armOnSettle_ = true;
    triggerEdge_ = true;  // Note-On instant, for the scheduler's fixed-delay anchor
    // Already settled while prepared: arm the deferred pluck right away.
    if (state_ == StringState::ReadyToPluck) pluckArmed_ = true;
    return true;
}

void StringController::motionReached() {
    // Finger is released -> move -> arrive.
    if (state_ == StringState::ReleasingFinger) {
        state_ = StringState::Moving;
    }
    if (state_ == StringState::Moving) {
        if (openString_) {
            // Open string: no finger press, ready immediately (spec 15.3).
            state_ = StringState::ReadyToPluck;
            pluckArmed_ = armOnSettle_;  // stay disarmed if merely prepared
        } else {
            state_ = StringState::PressingFinger;
        }
    }
}

void StringController::fingerPressed() {
    if (state_ == StringState::PressingFinger) {
        state_ = StringState::Settling;
    }
}

void StringController::settled() {
    if (state_ == StringState::Settling) {
        state_ = StringState::ReadyToPluck;
        // Deferred pluck armed (tagged by commandId_) — unless this is a merely
        // prepared note, which stays disarmed until trigger().
        pluckArmed_ = armOnSettle_;
    }
}

bool StringController::executePluck(uint32_t id) {
    // Guard: stale / cancelled commands are ignored (spec section 16).
    if (id != commandId_) return false;
    if (state_ != StringState::ReadyToPluck || !pluckArmed_) return false;
    pluckArmed_ = false;
    state_ = StringState::Plucking;
    state_ = StringState::Sustaining;
    return true;
}

void StringController::noteOff() {
    if (state_ == StringState::Disabled || state_ == StringState::Fault) return;
    // Any attack still in preparation is cancelled; a sounding note is damped.
    bool wasSounding =
        state_ == StringState::Sustaining || state_ == StringState::Plucking;
    invalidate();  // kills any armed pluck (spec: no delayed pluck after Note Off)
    state_ = wasSounding ? StringState::Damping : StringState::Idle;
}

void StringController::dampingDone() {
    if (state_ == StringState::Damping) state_ = StringState::Idle;
}

void StringController::panic() {
    // A disabled or faulted axis stays out of service — a panic (CC120/123,
    // Wi-Fi loss, software panic) must never resurrect it to Idle.
    if (state_ == StringState::Disabled || state_ == StringState::Fault) {
        invalidate();
        return;
    }
    invalidate();
    state_ = StringState::Idle;
}

}  // namespace gmb
