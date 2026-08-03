// Per-string state machine (spec section 16).
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
    // Clear a runtime fault so the axis can be re-homed on an explicit reset. A
    // Disabled axis stays disabled (audit P0-3).
    void clearFault() { if (state_ == StringState::Fault) { state_ = StringState::Idle; invalidate(); } }

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

    // True for a normal note that arms its pluck as soon as it is ready; false
    // while a note is only PREPARED (anticipated) and still waiting for trigger().
    // Lets the scheduler skip strum-lift anticipation for a merely-prepared note
    // and re-anchor the fixed execution delay to the trigger instant.
    bool willArmOnSettle() const { return armOnSettle_; }

    // One-shot edge: true exactly once after trigger() fires for a prepared note,
    // so the scheduler can anchor the fixed execution delay to the Note-On instant
    // even when the note is triggered before it is mechanically ready.
    bool consumeTriggerEdge() { bool e = triggerEdge_; triggerEdge_ = false; return e; }

    // Release the note. Cancels any armed/prepared attack.
    void noteOff();

    // Damping finished -> back to idle.
    void dampingDone();

    // Emergency stop (spec 21.3).
    void panic();

private:
    StringState state_ = StringState::Disabled;
    uint32_t commandId_ = 0;
    uint32_t nextId_ = 1;
    int targetFret_ = 0;
    bool openString_ = true;
    bool pluckArmed_ = false;
    bool armOnSettle_ = true;  // false while a note is only prepared (not triggered)
    bool triggerEdge_ = false; // set by trigger(), consumed once by the scheduler

    void invalidate() {
        commandId_ = nextId_++;  // any deferred action tagged with the old id dies
        pluckArmed_ = false;
        armOnSettle_ = true;     // a fresh command arms normally by default
    }
};

}  // namespace gmb
