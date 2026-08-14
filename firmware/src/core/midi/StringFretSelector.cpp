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

// Offset + one-based bias only: the value the range check then judges. Kept
// separate so the Clamp policy can bring an out-of-range index back in and run
// the SAME order/mapping rules on it, rather than reimplementing them.
int StringFretSelector::logicalStringIndex(uint8_t rawValue) const {
    int logical = static_cast<int>(rawValue) + cfg_.string.offset;
    if (cfg_.string.numbering == StringNumbering::OneBased) logical -= 1;
    return logical;
}

// Range check, order reversal and the custom mapping table.
int StringFretSelector::physicalAxisFor(int index) const {
    if (index < 0 || index >= instrument_.stringCount) return -1;
    if (cfg_.string.reverseOrder) index = (instrument_.stringCount - 1) - index;
    if (!cfg_.string.mapping.empty()) {
        if (index >= static_cast<int>(cfg_.string.mapping.size())) return -1;
        index = cfg_.string.mapping[index];
    }
    if (index < 0 || index >= instrument_.stringCount) return -1;
    return index;
}

int StringFretSelector::mapStringValue(uint8_t rawValue) const {
    // Apply the offset FIRST, then validate the LOGICAL value against the allowed
    // range (STRING_FRET_SELECTION.md: "logical = CC + offset, then validated").
    // Validating the raw value instead shifts the accepted band away from
    // [min,max] as soon as the offset is non-zero, so a configuration with an
    // offset silently accepts the wrong CC values and rejects the right ones.
    const int logical = static_cast<int>(rawValue) + cfg_.string.offset;
    if (logical < cfg_.string.minimum || logical > cfg_.string.maximum) return -1;
    return physicalAxisFor(logicalStringIndex(rawValue));
}

int StringFretSelector::mapFretValue(uint8_t rawValue) const {
    // Offset first, then validate the logical fret against [min,max] — same rule
    // as the string value above.
    int logical = static_cast<int>(rawValue) + cfg_.fret.offset;
    if (logical < 0) return -1;
    if (logical < cfg_.fret.minimum || logical > cfg_.fret.maximum) return -1;
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
            // Invalid string value: fill the string slot as INVALID rather than
            // dropping it silently. Bind a fret already waiting for its string
            // here, so it is not left orphaned for a LATER, unrelated string CC to
            // mis-pair with; the note is then resolved by the invalid-value policy
            // at Note On.
            const int16_t logical =
                static_cast<int16_t>(logicalStringIndex(e.data2));
            for (auto& s : pending_) {
                if (s.midiChannel == key && s.hasFret && !s.hasString) {
                    s.hasString = true;
                    s.invalid = true;
                    s.logicalString = logical;   // Clamp needs the rejected value
                    s.expiresAtUs = e.timestampUs + timeoutUs;
                    return true;
                }
            }
            if (pending_.size() >= cfg_.queueDepth) pending_.erase(pending_.begin());
            PendingStringSelection s;
            s.midiChannel = key;
            s.hasString = true;
            s.invalid = true;
            s.logicalString = logical;
            s.receivedAtUs = e.timestampUs;
            s.expiresAtUs = e.timestampUs + timeoutUs;
            pending_.push_back(s);
            return true;
        }
        // If a fret arrived first, complete that oldest fret-only entry instead
        // of opening a new one (symmetric with the fret branch below).
        for (auto& s : pending_) {
            if (s.midiChannel == key && s.hasFret && !s.hasString) {
                s.hasString = true;
                s.stringValue = static_cast<uint8_t>(axis);
                s.expiresAtUs = e.timestampUs + timeoutUs;
                noteMaybePrepare(s);  // string completed a waiting fret
                return true;
            }
        }
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
            // Invalid fret value: mirror the string branch — fill the fret slot as
            // INVALID, binding a string already waiting for its fret.
            const int16_t logical =
                static_cast<int16_t>(static_cast<int>(e.data2) + cfg_.fret.offset);
            for (auto& s : pending_) {
                if (s.midiChannel == key && s.hasString && !s.hasFret) {
                    s.hasFret = true;
                    s.invalid = true;
                    s.logicalFret = logical;     // Clamp needs the rejected value
                    s.expiresAtUs = e.timestampUs + timeoutUs;
                    return true;
                }
            }
            if (pending_.size() >= cfg_.queueDepth) pending_.erase(pending_.begin());
            PendingStringSelection s;
            s.midiChannel = key;
            s.hasFret = true;
            s.invalid = true;
            s.logicalFret = logical;
            s.receivedAtUs = e.timestampUs;
            s.expiresAtUs = e.timestampUs + timeoutUs;
            pending_.push_back(s);
            return true;
        }
        // Attach to the oldest string-selection on this channel still missing a
        // fret (spec section 9).
        for (auto& s : pending_) {
            if (s.midiChannel == key && s.hasString && !s.hasFret) {
                s.hasFret = true;
                s.fretValue = static_cast<uint8_t>(fret);
                s.expiresAtUs = e.timestampUs + timeoutUs;
                noteMaybePrepare(s);  // fret completed a waiting string
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

void StringFretSelector::noteMaybePrepare(const PendingStringSelection& s) {
    if (!cfg_.prepareOnCompleteSelection || !s.complete()) return;
    // Only pre-position selections that are physically valid — an out-of-range
    // string/fret is resolved (rejected / fallback) at Note On time, not moved.
    if (s.stringValue >= instrument_.stringCount) return;
    if (s.fretValue > instrument_.maxFret(s.stringValue)) return;
    // Carry the selection's own expiry so the preparation is released in step with
    // the selection, not after a fixed unrelated window (audit P1-6).
    justCompleted_.push_back({s.midiChannel, s.stringValue, s.fretValue, s.expiresAtUs});
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
    if (!cfg_.enabled || cfg_.mode == SelectionMode::Automatic) {
        expire(nowUs);
        return automaticResolution();
    }

    const uint8_t key = channelKey(e.channel);

    // Find the oldest complete selection for this channel (spec section 9).
    // Prefer the oldest NON-expired one, and remember an expired one only as a
    // fallback: taking the first complete entry regardless lets a stale selection
    // shadow a newer, still-valid one that arrived behind it, so a correct note
    // gets rejected because an older one timed out. Expiry is still surfaced
    // through expiredSelectionPolicy, never silently treated as "missing".
    int idx = -1, expiredIdx = -1;
    for (size_t i = 0; i < pending_.size(); ++i) {
        if (pending_[i].midiChannel != key || !pending_[i].complete()) continue;
        if (static_cast<int32_t>(nowUs - pending_[i].expiresAtUs) >= 0) {
            if (expiredIdx < 0) expiredIdx = static_cast<int>(i);
        } else {
            idx = static_cast<int>(i);
            break;
        }
    }

    auto applyPolicy = [&](InvalidValuePolicy policy,
                           const char* why) -> NoteResolution {
        switch (policy) {
            case InvalidValuePolicy::AutomaticFallback:
                return automaticResolution();
            case InvalidValuePolicy::LastValid:
                if (lastValid_[key].valid) {
                    NoteResolution r;
                    r.play = true;
                    r.source = ResolveSource::Explicit;
                    r.stringIndex = lastValid_[key].stringIndex;
                    r.fret = lastValid_[key].fret;
                    r.noteInstanceId = nextInstanceId_++;
                    active_.push_back({e.channel, e.data1, r.stringIndex, r.fret,
                                       r.noteInstanceId});
                    return r;
                }
                return automaticResolution();
            case InvalidValuePolicy::Clamp:
            case InvalidValuePolicy::Reject:
            default: {
                NoteResolution r;
                r.play = false;
                r.source = ResolveSource::Rejected;
                r.warning = why;
                return r;
            }
        }
    };

    if (idx < 0) {
        // No usable (non-expired) selection. If an expired one is present, apply
        // the expired policy; otherwise the missing-selection policy. Hybrid mode
        // always falls back to automatic allocation.
        if (expiredIdx >= 0) {
            pending_.erase(pending_.begin() + expiredIdx);
            expire(nowUs);
            if (cfg_.mode == SelectionMode::Hybrid &&
                cfg_.expiredSelectionPolicy != InvalidValuePolicy::Reject) {
                return automaticResolution();
            }
            return applyPolicy(cfg_.expiredSelectionPolicy, "string/fret selection expired");
        }
        expire(nowUs);
        if (cfg_.mode == SelectionMode::Hybrid) return automaticResolution();
        return applyPolicy(cfg_.missingSelectionPolicy,
                           "incomplete string/fret selection");
    }

    PendingStringSelection sel = pending_[idx];
    pending_.erase(pending_.begin() + idx);

    uint8_t stringIndex = sel.stringValue;
    uint8_t fret = sel.fretValue;

    // Range validation against the instrument. A selection carrying an
    // out-of-range CC value is routed through the invalid-value policy instead of
    // being played.
    bool valid = !sel.invalid &&
                 stringIndex < instrument_.stringCount &&
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
                // Clamp the value that was actually REQUESTED. stringValue /
                // fretValue only hold the value of a CC that passed validation —
                // for a rejected one they are still 0, so clamping them turned
                // "fret 127, bring it into range" into "fret 0": the far end of
                // the fretboard, on a carriage that then really travels there.
                // The logical values recorded at CC time are what to clamp.
                if (sel.logicalString != PendingStringSelection::kNoValue) {
                    int idxLogical = sel.logicalString;
                    if (idxLogical < 0) idxLogical = 0;
                    if (idxLogical >= instrument_.stringCount)
                        idxLogical = instrument_.stringCount - 1;
                    // Back through the SAME order/mapping rules a valid value takes.
                    int axis = physicalAxisFor(idxLogical);
                    if (axis >= 0) stringIndex = static_cast<uint8_t>(axis);
                }
                if (stringIndex >= instrument_.stringCount)
                    stringIndex = instrument_.stringCount - 1;
                if (sel.logicalFret != PendingStringSelection::kNoValue) {
                    int fretLogical = sel.logicalFret;
                    if (fretLogical < 0) fretLogical = 0;
                    fret = static_cast<uint8_t>(fretLogical > 127 ? 127 : fretLogical);
                }
                uint8_t mf = instrument_.maxFret(stringIndex);
                if (fret > mf) fret = mf;
                break;
            }
            case InvalidValuePolicy::LastValid:
                if (lastValid_[key].valid) {
                    stringIndex = lastValid_[key].stringIndex;
                    fret = lastValid_[key].fret;
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

    // Remember this fully-validated string+fret PAIR for the LastValid policy on
    // THIS channel (never mixed across channels or across two selections).
    lastValid_[key] = {true, stringIndex, fret};

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
        // Overflow-safe comparison: micros() wraps ~every 71 min, so compare the
        // signed difference rather than using >= directly.
        if (static_cast<int32_t>(nowUs - it->expiresAtUs) >= 0) {
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace gmb
