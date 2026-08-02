// Per-string state machine (cahier des charges section 16).
//
// Each string runs an independent, non-blocking state machine. Every command
// carries an id; when a command is cancelled or replaced, any deferred action
// tagged with the old id is ignored. This prevents a pluck after a Note Off, a
// late finger press, execution of a stale position, or an attack after a panic.
#pragma once

#include <cstdint>

namespace gmb {

enum class StringState : uint8_t {
    Disabled,
    Homing,
    Idle,
    ReleasingFinger,
    Moving,
    PressingFinger,
    Settling,
    ReadyToPluck,
    Plucking,
    Sustaining,
    Damping,
    Cancelling,
    Fault,
};

class StringController {
public:
    StringState state() const { return state_; }
    uint32_t commandId() const { return commandId_; }
    int targetFret() const { return targetFret_; }
    bool openString() const { return openString_; }

    void enable() { if (state_ == StringState::Disabled) state_ = StringState::Idle; }
    void disable() { state_ = StringState::Disabled; invalidate(); }
    // A disabled or faulted axis must never be dragged into homing.
    void setHoming() {
        if (state_ != StringState::Disabled && state_ != StringState::Fault)
            state_ = StringState::Homing;
    }
    void homingDone() { if (state_ == StringState::Homing) state_ = StringState::Idle; }
    void fault() { state_ = StringState::Fault; invalidate(); }

    // Begin a new note. Returns the fresh command id. Cancels any prior pending
    // action by advancing the command id. The pluck is armed automatically once
    // the string is ready.
    uint32_t noteOn(int fret);

    // Anticipated preparation (prepareOnCompleteSelection): move + press exactly
    // like noteOn, but DO NOT arm the pluck when ready — it waits for trigger().
    uint32_t prepareNote(int fret);

    // Arm the (previously prepared) pluck. Runs only for the matching command;
    // if the string is not yet ready it arms as soon as it settles.
    bool trigger(uint32_t id);

    // Progress hooks driven by motion/servo/timers (non-blocking).
    void motionReached();
    void fingerPressed();
    void settled();

    // Execute the deferred pluck. Runs only when `id` still matches the current
    // command and the string is ready — otherwise it is silently ignored.
    bool executePluck(uint32_t id);

    // Whether a deferred pluck is currently armed (for the scheduler).
    bool pluckArmed() const { return pluckArmed_; }
    uint32_t pluckCommandId() const { return commandId_; }

    // Release the note. Cancels any armed/prepared attack.
    void noteOff();

    // Damping finished -> back to idle.
    void dampingDone();

    // Emergency stop (cahier des charges 21.3).
    void panic();

private:
    StringState state_ = StringState::Disabled;
    uint32_t commandId_ = 0;
    uint32_t nextId_ = 1;
    int targetFret_ = 0;
    bool openString_ = true;
    bool pluckArmed_ = false;
    bool armOnSettle_ = true;  // false while a note is only prepared (not triggered)

    void invalidate() {
        commandId_ = nextId_++;  // any deferred action tagged with the old id dies
        pluckArmed_ = false;
        armOnSettle_ = true;     // a fresh command arms normally by default
    }
};

}  // namespace gmb
