#include "InstrumentController.h"

namespace gmb {

void InstrumentController::load(const Profile& p) {
    strings_.clear();
    axes_.clear();
    targets_.clear();
    active_.clear();

    selector_.configure(p.selector);
    selector_.setInstrument(p.instrumentView());

    std::vector<StringSpec> specs;
    for (const auto& s : p.strings) {
        StringSpec spec;
        spec.openNote = s.openNote;
        spec.maxFret = s.maxFret;
        spec.enabled = s.enabled;
        specs.push_back(spec);

        axes_.emplace_back(s);
        StringController c;
        c.enable();
        strings_.push_back(c);
        targets_.push_back(StringTarget{});
    }
    allocator_.setStrings(specs);
    allocator_.setStrategy(SaturationStrategy::PriorityLow);
}

int InstrumentController::soundingCount() const {
    int n = 0;
    for (const auto& t : targets_)
        if (t.active) ++n;
    return n;
}

void InstrumentController::startNote(int stringIndex, int fret, uint8_t channel,
                                     uint8_t note) {
    if (stringIndex < 0 || stringIndex >= static_cast<int>(strings_.size())) return;
    uint32_t id = strings_[stringIndex].noteOn(fret);
    if (id == 0) return;
    targets_[stringIndex].active = true;
    targets_[stringIndex].fret = fret;
    targets_[stringIndex].positionMm = axes_[stringIndex].fretPositionMm(fret);
    targets_[stringIndex].commandId = id;
    active_.push_back({channel, note, stringIndex});
}

void InstrumentController::stopString(int stringIndex) {
    if (stringIndex < 0 || stringIndex >= static_cast<int>(strings_.size())) return;
    strings_[stringIndex].noteOff();
    targets_[stringIndex].active = false;
    allocator_.release(stringIndex);
}

int InstrumentController::popActive(uint8_t channel, uint8_t note) {
    for (int i = static_cast<int>(active_.size()) - 1; i >= 0; --i) {
        if (active_[i].channel == channel && active_[i].note == note) {
            int idx = active_[i].stringIndex;
            active_.erase(active_.begin() + i);
            return idx;
        }
    }
    return -1;
}

void InstrumentController::handleEvent(const MidiEvent& e, uint32_t nowUs) {
    if (e.isControlChange()) {
        // Channel-mode: all sound / notes off (cahier des charges §21).
        if (e.data1 == 120 || e.data1 == 123) {
            panic();
            return;
        }
        selector_.onControlChange(e);
        return;
    }

    if (e.isNoteOn()) {
        NoteResolution r = selector_.onNoteOn(e, nowUs);
        if (!r.play) return;  // rejected by strict/explicit policy
        if (r.source == ResolveSource::Explicit) {
            startNote(r.stringIndex, r.fret, e.channel, e.data1);
        } else {
            // Automatic allocation.
            uint8_t fret = 0;
            int8_t s = allocator_.allocateOne(e.data1, &fret);
            if (s >= 0) startNote(s, fret, e.channel, e.data1);
        }
        return;
    }

    if (e.isNoteOff()) {
        // Prefer the selector's explicit record; fall back to our own map.
        ActiveNote a;
        int stringIndex = -1;
        if (selector_.onNoteOff(e, &a)) {
            stringIndex = a.stringIndex;
            popActive(e.channel, e.data1);  // keep maps in sync
        } else {
            stringIndex = popActive(e.channel, e.data1);
        }
        if (stringIndex >= 0) stopString(stringIndex);
        return;
    }
}

void InstrumentController::panic() {
    for (auto& s : strings_) s.panic();
    for (auto& t : targets_) t.active = false;
    for (size_t i = 0; i < strings_.size(); ++i) allocator_.release(static_cast<int>(i));
    active_.clear();
}

}  // namespace gmb
