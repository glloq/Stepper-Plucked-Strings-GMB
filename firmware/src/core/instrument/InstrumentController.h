// Ties the MIDI pipeline together (cahier des charges §16/§17, selection spec).
//
// transport -> MidiEvent -> [selection] -> [allocation] -> per-string state
// machine -> motion/servo commands. Pure C++: the platform layer feeds it MIDI
// events and reads back the resulting per-string targets.
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
};

class InstrumentController {
public:
    // Build the runtime from an active, validated profile.
    void load(const Profile& p);

    // Feed one decoded MIDI event.
    void handleEvent(const MidiEvent& e, uint32_t nowUs);

    // Emergency stop everything (cahier des charges §21.3).
    void panic();

    size_t stringCount() const { return strings_.size(); }
    const StringController& string(size_t i) const { return strings_[i]; }
    StringController& string(size_t i) { return strings_[i]; }
    const StringTarget& target(size_t i) const { return targets_[i]; }

    // How many strings are currently holding a note.
    int soundingCount() const;

private:
    StringFretSelector selector_;
    NoteAllocator allocator_;
    std::vector<StringController> strings_;
    std::vector<StepperAxis> axes_;
    std::vector<StringTarget> targets_;

    struct ActiveMap {
        uint8_t channel;
        uint8_t note;
        int stringIndex;
    };
    std::vector<ActiveMap> active_;

    void startNote(int stringIndex, int fret, uint8_t channel, uint8_t note);
    void stopString(int stringIndex);
    int popActive(uint8_t channel, uint8_t note);
};

}  // namespace gmb
