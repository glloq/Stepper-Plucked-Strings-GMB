// Explicit string/fret selection over MIDI CC (spec: "selection corde et frette").
//
// General-Midi-Boop (or a tablature-aware MIDI file) sends a string CC (default
// CC20) and a fret CC (default CC21) *before* a Note On to force a physical
// tablature position. This module reconstructs per-channel selections into a
// FIFO so chords stay correct, associates Note Ons in order, and remembers the
// real assignment so Note Off releases the right string.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../Types.h"
#include "MidiEvent.h"

namespace gmb {

enum class SelectionMode : uint8_t {
    Automatic = 0,  // ignore CCs, always allocate automatically
    Explicit = 1,   // string & fret forced by CC
    Hybrid = 2,     // CC when valid, automatic fallback (default)
};

enum class StringNumbering : uint8_t { ZeroBased = 0, OneBased = 1 };

enum class InvalidValuePolicy : uint8_t {
    Reject = 0,
    Clamp = 1,
    AutomaticFallback = 2,  // default
    LastValid = 3,
};

enum class NotePositionPolicy : uint8_t {
    CcPriorityWithWarning = 0,  // default: trust CC, warn on mismatch
    NotePriority = 1,           // recompute fret from the MIDI note
    Strict = 2,                 // reject incoherent note/string/fret
};

struct StringSelectionConfig {
    uint8_t ccNumber = 20;
    uint8_t minimum = 1;
    uint8_t maximum = kMaxStrings;
    int8_t offset = 0;
    StringNumbering numbering = StringNumbering::OneBased;
    bool reverseOrder = false;
    std::vector<int8_t> mapping;  // logical index -> physical axis, empty = identity
};

struct FretSelectionConfig {
    uint8_t ccNumber = 21;
    uint8_t minimum = 0;
    uint8_t maximum = 24;
    int8_t offset = 0;
    InvalidValuePolicy invalidValuePolicy = InvalidValuePolicy::AutomaticFallback;
};

struct SelectorConfig {
    bool enabled = true;
    SelectionMode mode = SelectionMode::Hybrid;
    bool perMidiChannel = true;
    uint32_t selectionTimeoutMs = 100;   // 5..2000
    bool prepareOnCompleteSelection = true;
    uint16_t queueDepth = 32;            // >= 16
    StringSelectionConfig string;
    FretSelectionConfig fret;
    NotePositionPolicy notePositionPolicy = NotePositionPolicy::CcPriorityWithWarning;
    InvalidValuePolicy missingSelectionPolicy = InvalidValuePolicy::AutomaticFallback;
    InvalidValuePolicy expiredSelectionPolicy = InvalidValuePolicy::AutomaticFallback;
};

// Instrument facts needed to validate an explicit selection.
struct InstrumentView {
    uint8_t stringCount = 4;
    std::vector<uint8_t> openNotes;      // MIDI note of each open string (physical order)
    std::vector<uint8_t> maxFretPerString;
    int8_t capo = 0;
    int8_t transpose = 0;

    uint8_t maxFret(uint8_t stringIndex) const {
        if (stringIndex < maxFretPerString.size()) return maxFretPerString[stringIndex];
        return 0;
    }
};

// One pending CC-driven selection (spec section 7).
struct PendingStringSelection {
    uint8_t midiChannel = 0;
    bool hasString = false;
    bool hasFret = false;
    uint8_t stringValue = 0;   // physical axis index (already mapped)
    uint8_t fretValue = 0;
    uint32_t receivedAtUs = 0;
    uint32_t expiresAtUs = 0;
    bool complete() const { return hasString && hasFret; }
};

// A currently-sounding note, remembered so Note Off releases the right string
// (spec section 12).
struct ActiveNote {
    uint8_t midiChannel = 0;
    uint8_t midiNote = 0;
    uint8_t stringIndex = 0;
    uint8_t fret = 0;
    uint32_t noteInstanceId = 0;
};

// A CC selection that just became complete (string + fret both received). When
// prepareOnCompleteSelection is on, the instrument pre-positions this string in
// anticipation of the Note On (spec: "préparation anticipée").
struct CompletedSelection {
    uint8_t midiChannel = 0;
    uint8_t stringIndex = 0;  // physical axis (already mapped, range-checked)
    uint8_t fret = 0;
    uint32_t expiresAtUs = 0; // same expiry as the pending selection it came from
};

enum class ResolveSource : uint8_t { Explicit, Automatic, Rejected };

struct NoteResolution {
    bool play = false;
    ResolveSource source = ResolveSource::Automatic;
    uint8_t stringIndex = 0;
    uint8_t fret = 0;
    uint32_t noteInstanceId = 0;
    std::string warning;  // non-fatal note (e.g. note/fret mismatch)
};

class StringFretSelector {
public:
    void configure(const SelectorConfig& cfg) { cfg_ = cfg; }
    const SelectorConfig& config() const { return cfg_; }
    void setInstrument(const InstrumentView& v) { instrument_ = v; }

    // Applies the General-Midi-Boop preset (spec section 3), adapting ranges to
    // the active instrument.
    void applyGmbPreset();

    // Feed a control-change event. Returns true if it was a selection CC that
    // was consumed by this module.
    bool onControlChange(const MidiEvent& e);

    // Resolve a Note On to a physical string/fret. `now` drives expiry.
    NoteResolution onNoteOn(const MidiEvent& e, uint32_t nowUs);

    // Look up the assignment made for a Note On so the right string is released.
    // Returns false if the note was never tracked.
    bool onNoteOff(const MidiEvent& e, ActiveNote* out);

    // Selections that became complete since the last call (empty unless
    // prepareOnCompleteSelection is enabled). The caller pre-positions them and
    // the list is cleared.
    std::vector<CompletedSelection> takeJustCompleted() {
        std::vector<CompletedSelection> out;
        out.swap(justCompleted_);
        return out;
    }

    // Drop expired pending selections (spec section 4.4). Call periodically.
    void expire(uint32_t nowUs);

    // Clear all pending/active selections and last-valid state. Call on profile
    // load, panic, or tuning/string-count change so stale CC selections from a
    // previous configuration are never applied to the new one.
    void reset() {
        pending_.clear();
        active_.clear();
        for (auto& lv : lastValid_) lv = LastValidSelection{};
        nextInstanceId_ = 1;
        justCompleted_.clear();
    }

    const std::vector<PendingStringSelection>& pending() const { return pending_; }
    const std::vector<ActiveNote>& active() const { return active_; }

    // Value transforms exposed for the web MIDI monitor / tests.
    // Returns the physical axis index for a raw string-CC value, or -1 if invalid.
    int mapStringValue(uint8_t rawValue) const;
    // Returns the logical fret for a raw fret-CC value, or -1 if invalid.
    int mapFretValue(uint8_t rawValue) const;

private:
    SelectorConfig cfg_;
    InstrumentView instrument_;
    std::vector<PendingStringSelection> pending_;
    std::vector<ActiveNote> active_;
    uint32_t nextInstanceId_ = 1;
    // Last fully-validated string+fret PAIR, remembered PER channel key so the
    // LastValid policy on channel 2 never reuses a value seen on channel 1, and
    // the string/fret always come from the same validated selection (audit P1-5).
    struct LastValidSelection {
        bool valid = false;
        uint8_t stringIndex = 0;
        uint8_t fret = 0;
    };
    LastValidSelection lastValid_[16];
    std::vector<CompletedSelection> justCompleted_;

    uint8_t channelKey(uint8_t ch) const { return cfg_.perMidiChannel ? ch : 0; }
    // Record a newly-complete selection for anticipated pre-positioning, if the
    // feature is on and the selection is in range.
    void noteMaybePrepare(const PendingStringSelection& s);
    NoteResolution automaticResolution() const;
    bool coherent(uint8_t note, uint8_t stringIndex, uint8_t fret, std::string* warn) const;
};

}  // namespace gmb
