#include "StringFretSelector.h"

#include "../Types.h"

namespace gmb {

void StringFretSelector::applyGmbPreset() {
    cfg_.enabled = true;
    cfg_.mode = SelectionMode::Hybrid;
    cfg_.perMidiChannel = true;
    cfg_.prepareOnCompleteSelection = true;
    cfg_.string.ccNumber = 20;
    cfg_.string.minimum = 1;
    cfg_.string.maximum = instrument_.stringCount;
    cfg_.string.offset = 0;
    cfg_.string.numbering = StringNumbering::OneBased;
    cfg_.string.reverseOrder = false;
    cfg_.fret.ccNumber = 21;
    cfg_.fret.minimum = 0;
    // Maximum playable fret across all strings.
    uint8_t maxFret = 0;
    for (uint8_t f : instrument_.maxFretPerString) maxFret = f > maxFret ? f : maxFret;
    cfg_.fret.maximum = maxFret;
    cfg_.fret.offset = 0;
}

int StringFretSelector::mapStringValue(uint8_t rawValue) const {
    int logical = static_cast<int>(rawValue) + cfg_.string.offset;
    if (rawValue < cfg_.string.minimum || rawValue > cfg_.string.maximum) return -1;

    int index = logical;
    if (cfg_.string.numbering == StringNumbering::OneBased) index -= 1;
    if (index < 0 || index >= instrument_.stringCount) return -1;

    if (cfg_.string.reverseOrder) index = (instrument_.stringCount - 1) - index;

    if (!cfg_.string.mapping.empty()) {
        if (index >= static_cast<int>(cfg_.string.mapping.size())) return -1;
        index = cfg_.string.mapping[index];
    }
    if (index < 0 || index >= instrument_.stringCount) return -1;
    return index;
}

int StringFretSelector::mapFretValue(uint8_t rawValue) const {
    if (rawValue < cfg_.fret.minimum || rawValue > cfg_.fret.maximum) return -1;
    int logical = static_cast<int>(rawValue) + cfg_.fret.offset;
    if (logical < 0) return -1;
    return logical;
}

bool StringFretSelector::onControlChange(const MidiEvent& e) {
    if (!cfg_.enabled || cfg_.mode == SelectionMode::Automatic) return false;
    if (!e.isControlChange()) return false;

    const uint32_t timeoutUs = cfg_.selectionTimeoutMs * 1000u;
    const uint8_t key = channelKey(e.channel);

    if (e.data1 == cfg_.string.ccNumber) {
        int axis = mapStringValue(e.data2);
        if (axis < 0) {
            lastValidString_ = -1;
            return true;  // consumed, but invalid — handled at Note On time
        }
        lastValidString_ = axis;
        // New selection in the FIFO (spec section 9).
        if (pending_.size() >= cfg_.queueDepth) pending_.erase(pending_.begin());
        PendingStringSelection s;
        s.midiChannel = key;
        s.hasString = true;
        s.stringValue = static_cast<uint8_t>(axis);
        s.receivedAtUs = e.timestampUs;
        s.expiresAtUs = e.timestampUs + timeoutUs;
        pending_.push_back(s);
        return true;
    }

    if (e.data1 == cfg_.fret.ccNumber) {
        int fret = mapFretValue(e.data2);
        if (fret < 0) {
            lastValidFret_ = -1;
            return true;
        }
        lastValidFret_ = fret;
        // Attach to the oldest string-selection on this channel still missing a
        // fret (spec section 9).
        for (auto& s : pending_) {
            if (s.midiChannel == key && s.hasString && !s.hasFret) {
                s.hasFret = true;
                s.fretValue = static_cast<uint8_t>(fret);
                s.expiresAtUs = e.timestampUs + timeoutUs;
                return true;
            }
        }
        // Fret arrived before its string: stash a fret-only pending entry.
        if (pending_.size() >= cfg_.queueDepth) pending_.erase(pending_.begin());
        PendingStringSelection s;
        s.midiChannel = key;
        s.hasFret = true;
        s.fretValue = static_cast<uint8_t>(fret);
        s.receivedAtUs = e.timestampUs;
        s.expiresAtUs = e.timestampUs + timeoutUs;
        pending_.push_back(s);
        return true;
    }

    return false;
}

bool StringFretSelector::coherent(uint8_t note, uint8_t stringIndex, uint8_t fret,
                                  std::string* warn) const {
    if (stringIndex >= instrument_.openNotes.size()) return false;
    int expected = frettedNote(instrument_.openNotes[stringIndex], fret,
                               instrument_.capo, instrument_.transpose);
    if (expected != static_cast<int>(note) && warn) {
        *warn = "note/fret mismatch: expected MIDI " + std::to_string(expected) +
                ", received " + std::to_string(note);
    }
    return expected == static_cast<int>(note);
}

NoteResolution StringFretSelector::automaticResolution() const {
    NoteResolution r;
    r.play = true;
    r.source = ResolveSource::Automatic;
    return r;
}

NoteResolution StringFretSelector::onNoteOn(const MidiEvent& e, uint32_t nowUs) {
    expire(nowUs);

    if (!cfg_.enabled || cfg_.mode == SelectionMode::Automatic) {
        return automaticResolution();
    }

    const uint8_t key = channelKey(e.channel);

    // Find the oldest complete selection for this channel (spec section 9).
    int idx = -1;
    for (size_t i = 0; i < pending_.size(); ++i) {
        if (pending_[i].midiChannel == key && pending_[i].complete()) {
            idx = static_cast<int>(i);
            break;
        }
    }

    if (idx < 0) {
        // No usable explicit selection.
        if (cfg_.mode == SelectionMode::Hybrid ||
            cfg_.missingSelectionPolicy == InvalidValuePolicy::AutomaticFallback) {
            return automaticResolution();
        }
        NoteResolution r;
        r.play = false;
        r.source = ResolveSource::Rejected;
        r.warning = "incomplete string/fret selection";
        return r;
    }

    PendingStringSelection sel = pending_[idx];
    pending_.erase(pending_.begin() + idx);

    uint8_t stringIndex = sel.stringValue;
    uint8_t fret = sel.fretValue;

    // Range validation against the instrument.
    bool valid = stringIndex < instrument_.stringCount &&
                 fret <= instrument_.maxFret(stringIndex);

    // Note/string/fret coherence (spec section 11).
    std::string warn;
    if (valid) {
        coherent(e.data1, stringIndex, fret, &warn);
        switch (cfg_.notePositionPolicy) {
            case NotePositionPolicy::CcPriorityWithWarning:
                // Keep CC values, warning already captured.
                break;
            case NotePositionPolicy::NotePriority: {
                int recomputed = static_cast<int>(e.data1) -
                                 instrument_.openNotes[stringIndex] -
                                 instrument_.capo - instrument_.transpose;
                if (recomputed >= 0 && recomputed <= instrument_.maxFret(stringIndex)) {
                    fret = static_cast<uint8_t>(recomputed);
                    warn.clear();
                } else {
                    valid = false;
                }
                break;
            }
            case NotePositionPolicy::Strict:
                if (!warn.empty()) valid = false;
                break;
        }
    }

    if (!valid) {
        switch (cfg_.fret.invalidValuePolicy) {
            case InvalidValuePolicy::AutomaticFallback:
                return automaticResolution();
            case InvalidValuePolicy::Clamp: {
                if (stringIndex >= instrument_.stringCount)
                    stringIndex = instrument_.stringCount - 1;
                uint8_t mf = instrument_.maxFret(stringIndex);
                if (fret > mf) fret = mf;
                break;
            }
            case InvalidValuePolicy::LastValid:
                if (lastValidString_ >= 0 && lastValidFret_ >= 0) {
                    stringIndex = static_cast<uint8_t>(lastValidString_);
                    fret = static_cast<uint8_t>(lastValidFret_);
                } else {
                    return automaticResolution();
                }
                break;
            case InvalidValuePolicy::Reject: {
                NoteResolution r;
                r.play = false;
                r.source = ResolveSource::Rejected;
                r.warning = "invalid string/fret selection";
                return r;
            }
        }
    }

    NoteResolution r;
    r.play = true;
    r.source = ResolveSource::Explicit;
    r.stringIndex = stringIndex;
    r.fret = fret;
    r.warning = warn;
    r.noteInstanceId = nextInstanceId_++;

    // Remember for Note Off (spec section 12). Repeated identical pitches stack.
    active_.push_back({e.channel, e.data1, stringIndex, fret, r.noteInstanceId});
    return r;
}

bool StringFretSelector::onNoteOff(const MidiEvent& e, ActiveNote* out) {
    // Match the most recent active instance for this channel+note (LIFO stack of
    // repeated notes, spec section 12).
    for (int i = static_cast<int>(active_.size()) - 1; i >= 0; --i) {
        if (active_[i].midiChannel == e.channel && active_[i].midiNote == e.data1) {
            if (out) *out = active_[i];
            active_.erase(active_.begin() + i);
            return true;
        }
    }
    return false;
}

void StringFretSelector::expire(uint32_t nowUs) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (nowUs >= it->expiresAtUs) {
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace gmb
