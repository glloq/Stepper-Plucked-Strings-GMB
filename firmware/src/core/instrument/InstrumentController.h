// Ties the MIDI pipeline together (spec §16/§17/§18, selection spec).
//
// transport -> MidiEvent -> [channel filter] -> [selection] -> [chord grouping +
// allocation] -> per-string state machine -> motion/servo targets.
// Pure C++: the platform layer feeds it MIDI events, calls tick() to flush chord
// groups, and reads back the per-string targets.
#pragma once

#include <cstdint>
#include <vector>

#include "../configuration/Profile.h"
#include "../midi/MidiEvent.h"
#include "../midi/StringFretSelector.h"
#include "NoteAllocator.h"
#include "StringController.h"

namespace gmb {

struct StringTarget {
    bool active = false;
    int fret = 0;
    double positionMm = 0.0;
    uint32_t commandId = 0;
    uint8_t velocity = 0;    // raw MIDI velocity
    double intensity = 0.0;  // shaped 0..1 (velocity curve)
};

class InstrumentController {
public:
    void load(const Profile& p);

    // Feed one decoded MIDI event.
    void handleEvent(const MidiEvent& e, uint32_t nowUs);

    // Flush pending chord groups whose window has elapsed. Call every loop.
    void tick(uint32_t nowUs);

    // Emergency stop everything (spec §21.3).
    void panic();

    // Take a string out of service at runtime (failed homing, etc.): fault its
    // state machine, mark it faulted in the allocator, drop its target and any
    // active note. It can no longer be chosen automatically OR by explicit CC.
    void faultString(size_t index);
    // Undo a runtime fault so the axis can be re-homed and played again (explicit
    // reset). Clears the StringController fault and the allocator fault flag.
    void recoverString(size_t index);

    size_t stringCount() const { return strings_.size(); }
    const StringController& string(size_t i) const { return strings_[i]; }
    StringController& string(size_t i) { return strings_[i]; }
    const StringTarget& target(size_t i) const { return targets_[i]; }
    int soundingCount() const;
    bool pedalDown() const { return pedalDown_; }

private:
    StringFretSelector selector_;
    NoteAllocator allocator_;
    std::vector<StringController> strings_;
    std::vector<StepperAxis> axes_;
    std::vector<StringTarget> targets_;

    // MIDI runtime settings pulled from the profile.
    uint8_t channel_ = 0;
    bool omni_ = false;
    uint32_t chordWindowUs_ = 3000;
    bool sustainEnabled_ = true;
    uint8_t sustainCc_ = 64;
    int velocityCurve_ = 0;

    bool pedalDown_ = false;
    // Global attack gain from CC7 (volume) and CC11 (expression), 0..1 each. A
    // plucked string can't modulate a sustained note, so these scale the attack
    // intensity of subsequent plucks (spec section 7).
    double volume_ = 1.0;
    double expression_ = 1.0;
    double attackGain() const { return volume_ * expression_; }

    struct ActiveMap {
        uint8_t channel;
        uint8_t note;
        int stringIndex;
        bool heldByPedal = false;  // released while the sustain pedal was down
    };
    std::vector<ActiveMap> active_;

    struct PendingNote {
        uint8_t channel;
        uint8_t note;
        uint8_t velocity;
        uint32_t atUs;
    };
    std::vector<PendingNote> chordBuffer_;  // automatic notes awaiting grouping

    // Anticipated pre-positioning (prepareOnCompleteSelection): moves + presses a
    // string on a complete CC selection so the Note On only needs to arm the
    // pluck. Empty vectors / 0 command id mean "no prepared note on this string".
    std::vector<int> preparedFret_;        // per string, -1 = none
    std::vector<uint32_t> preparedId_;     // per string, 0 = none
    std::vector<uint32_t> preparedExpiryUs_; // per string, when the prepare expires

    bool accepts(uint8_t channel) const { return omni_ || channel == channel_; }
    void prepareString(int stringIndex, int fret, uint32_t expiresAtUs);
    // Trigger a previously prepared string for this Note On. Returns false if the
    // string was not prepared for this fret (the caller then starts a fresh note).
    bool triggerPreparedNote(int stringIndex, int fret, uint8_t channel,
                             uint8_t note, uint8_t velocity);
    void startNote(int stringIndex, int fret, uint8_t channel, uint8_t note,
                   uint8_t velocity);
    void stopString(int stringIndex);
    int findActive(uint8_t channel, uint8_t note) const;
    void removeActiveByString(int stringIndex);
    void flushChord();
};

}  // namespace gmb
