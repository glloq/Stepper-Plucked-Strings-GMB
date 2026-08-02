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
    // The finger always lifts before moving (also the resting state for an open
    // string).
    state_ = StringState::ReleasingFinger;
    return commandId_;
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
            pluckArmed_ = true;
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
        pluckArmed_ = true;  // deferred pluck armed, tagged by commandId_
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
    if (state_ == StringState::Disabled) return;
    invalidate();
    state_ = StringState::Idle;
}

}  // namespace gmb
