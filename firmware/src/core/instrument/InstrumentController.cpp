#include "InstrumentController.h"

#include "../midi/Velocity.h"

namespace gmb {

void InstrumentController::load(const Profile& p) {
    strings_.clear();
    axes_.clear();
    targets_.clear();
    active_.clear();
    chordBuffer_.clear();
    pedalDown_ = false;

    selector_.configure(p.selector);
    selector_.setInstrument(p.instrumentView());

    channel_ = p.midi.globalChannel;
    omni_ = p.midi.omni;
    chordWindowUs_ = static_cast<uint32_t>(p.midi.chordWindowMs) * 1000u;
    sustainEnabled_ = p.midi.sustainPedal;
    sustainCc_ = p.midi.sustainCc;
    velocityCurve_ = static_cast<int>(p.midi.velocityCurve);

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
    allocator_.setStrategy(p.midi.saturationStrategy);
}

int InstrumentController::soundingCount() const {
    int n = 0;
    for (const auto& t : targets_)
        if (t.active) ++n;
    return n;
}

void InstrumentController::startNote(int stringIndex, int fret, uint8_t channel,
                                     uint8_t note, uint8_t velocity) {
    if (stringIndex < 0 || stringIndex >= static_cast<int>(strings_.size())) return;
    uint32_t id = strings_[stringIndex].noteOn(fret);
    if (id == 0) return;
    // Keep the allocator's view of busy strings in sync (explicit CC selections
    // bypass the allocator otherwise).
    if (stringIndex < static_cast<int>(allocator_.runtime().size())) {
        allocator_.runtime()[stringIndex].free = false;
        allocator_.runtime()[stringIndex].currentFret = fret;
    }
    StringTarget& t = targets_[stringIndex];
    t.active = true;
    t.fret = fret;
    t.positionMm = axes_[stringIndex].fretPositionMm(fret);
    t.commandId = id;
    t.velocity = velocity;
    t.intensity = applyVelocityCurve(velocityCurve_, velocity);
    active_.push_back({channel, note, stringIndex, false});
}

void InstrumentController::stopString(int stringIndex) {
    if (stringIndex < 0 || stringIndex >= static_cast<int>(strings_.size())) return;
    strings_[stringIndex].noteOff();
    targets_[stringIndex].active = false;
    allocator_.release(stringIndex);
}

int InstrumentController::findActive(uint8_t channel, uint8_t note) const {
    for (int i = static_cast<int>(active_.size()) - 1; i >= 0; --i) {
        if (active_[i].channel == channel && active_[i].note == note) return i;
    }
    return -1;
}

void InstrumentController::releaseNote(uint8_t channel, uint8_t note) {
    // Prefer the selector's explicit record; fall back to our own map.
    MidiEvent off;
    off.type = static_cast<uint8_t>(MidiType::NoteOff);
    off.channel = channel;
    off.data1 = note;
    ActiveNote a;
    selector_.onNoteOff(off, &a);

    int idx = findActive(channel, note);
    if (idx < 0) return;
    int stringIndex = active_[idx].stringIndex;
    active_.erase(active_.begin() + idx);
    stopString(stringIndex);
}

void InstrumentController::handleEvent(const MidiEvent& e, uint32_t nowUs) {
    if (!accepts(e.channel)) return;  // channel / omni filter (cahier §18)

    if (e.isControlChange()) {
        if (e.data1 == 120 || e.data1 == 123) {  // all sound / notes off
            panic();
            return;
        }
        if (sustainEnabled_ && e.data1 == sustainCc_) {
            bool down = e.data2 >= 64;
            if (pedalDown_ && !down) {
                // Pedal released: drop every note that was held by the pedal.
                for (int i = static_cast<int>(active_.size()) - 1; i >= 0; --i) {
                    if (active_[i].heldByPedal) {
                        int s = active_[i].stringIndex;
                        active_.erase(active_.begin() + i);
                        stopString(s);
                    }
                }
            }
            pedalDown_ = down;
            return;
        }
        selector_.onControlChange(e);
        return;
    }

    if (e.isNoteOn()) {
        NoteResolution r = selector_.onNoteOn(e, nowUs);
        if (!r.play) return;
        if (r.source == ResolveSource::Explicit) {
            startNote(r.stringIndex, r.fret, e.channel, e.data1, e.data2);
        } else {
            // Automatic allocation is deferred to group chord notes (§17.2).
            chordBuffer_.push_back({e.channel, e.data1, e.data2, nowUs});
            if (chordWindowUs_ == 0) flushChord();
        }
        return;
    }

    if (e.isNoteOff()) {
        int idx = findActive(e.channel, e.data1);
        if (idx >= 0 && pedalDown_ && sustainEnabled_) {
            active_[idx].heldByPedal = true;  // keep sounding until pedal up
            return;
        }
        releaseNote(e.channel, e.data1);
        return;
    }
}

void InstrumentController::flushChord() {
    if (chordBuffer_.empty()) return;
    std::vector<uint8_t> notes;
    notes.reserve(chordBuffer_.size());
    for (const auto& n : chordBuffer_) notes.push_back(n.note);

    std::vector<Allocation> alloc = allocator_.allocateChord(notes);
    for (const auto& a : alloc) {
        if (!a.assigned) continue;
        const PendingNote& src = chordBuffer_[a.index];
        // allocateChord already marked the string busy; record the note mapping
        // and command the string/motion.
        uint32_t id = strings_[a.stringIndex].noteOn(a.fret);
        if (id == 0) continue;
        StringTarget& t = targets_[a.stringIndex];
        t.active = true;
        t.fret = a.fret;
        t.positionMm = axes_[a.stringIndex].fretPositionMm(a.fret);
        t.commandId = id;
        t.velocity = src.velocity;
        t.intensity = applyVelocityCurve(velocityCurve_, src.velocity);
        active_.push_back({src.channel, src.note, a.stringIndex, false});
    }
    chordBuffer_.clear();
}

void InstrumentController::tick(uint32_t nowUs) {
    if (chordBuffer_.empty()) return;
    if (nowUs - chordBuffer_.front().atUs >= chordWindowUs_) flushChord();
}

void InstrumentController::panic() {
    for (auto& s : strings_) s.panic();
    for (auto& t : targets_) t.active = false;
    for (size_t i = 0; i < strings_.size(); ++i) allocator_.release(static_cast<int>(i));
    active_.clear();
    chordBuffer_.clear();
    pedalDown_ = false;
}

}  // namespace gmb
