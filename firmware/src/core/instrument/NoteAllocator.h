// Note -> string/fret allocation (cahier des charges section 17).
//
// Assigns incoming notes to the string that can play them with the least
// mechanical work, groups chord notes, and applies a saturation strategy when
// more notes are requested than there are free strings.
#pragma once

#include <cstdint>
#include <vector>

namespace gmb {

enum class SaturationStrategy : uint8_t {
    IgnoreExtra = 0,   // drop notes that don't fit
    PriorityLow = 1,   // keep the lowest notes
    PriorityHigh = 2,  // keep the highest notes
    PriorityFirst = 3, // keep the first received
    ReplaceOldest = 4, // steal the string sounding the oldest note
    Monophonic = 5,    // one note at a time
};

struct StringSpec {
    // Effective open pitch (open note + capo + transpose). SIGNED and NOT clamped
    // to the MIDI domain: a transposed reference can legitimately fall below 0 or
    // above 127, and clamping it would corrupt the fret math (audit P0-5).
    int16_t openNote = 0;
    uint8_t maxFret = 12;
    bool enabled = true;
    bool faulted = false;
};

struct StringRuntime {
    bool free = true;       // not currently holding a note
    int currentFret = 0;    // last commanded fret (movement cost reference)
    uint32_t noteAge = 0;   // ordering for ReplaceOldest (lower = older)
};

struct Allocation {
    int index = -1;    // note index in the request batch
    int8_t stringIndex = -1;
    uint8_t fret = 0;
    bool assigned = false;
};

class NoteAllocator {
public:
    void setStrings(const std::vector<StringSpec>& specs);
    void setRuntime(const std::vector<StringRuntime>& rt) { runtime_ = rt; }
    std::vector<StringRuntime>& runtime() { return runtime_; }
    void setStrategy(SaturationStrategy s) { strategy_ = s; }

    bool canPlay(int stringIndex, uint8_t note) const;
    int fretFor(int stringIndex, uint8_t note) const;

    // Mark a string faulted at runtime so it is never allocated again (and free
    // it if it was holding a note).
    void setFaulted(int stringIndex, bool faulted) {
        if (stringIndex >= 0 && stringIndex < static_cast<int>(specs_.size())) {
            specs_[stringIndex].faulted = faulted;
            if (faulted && stringIndex < static_cast<int>(runtime_.size()))
                runtime_[stringIndex].free = true;
        }
    }

    // Allocate one note to the best available string; -1 if none.
    // Marks the string busy on success.
    int8_t allocateOne(uint8_t note, uint8_t* fretOut = nullptr);

    // Allocate a chord (batch). Applies the saturation strategy. The returned
    // vector has one entry per requested note (assigned==false when dropped).
    std::vector<Allocation> allocateChord(const std::vector<uint8_t>& notes);

    void release(int stringIndex);

private:
    std::vector<StringSpec> specs_;
    std::vector<StringRuntime> runtime_;
    SaturationStrategy strategy_ = SaturationStrategy::PriorityLow;
    uint32_t ageCounter_ = 1;

    // Movement cost of playing `note` on `stringIndex` (lower is better).
    int cost(int stringIndex, uint8_t note) const;
};

}  // namespace gmb
