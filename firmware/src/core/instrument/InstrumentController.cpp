#include "InstrumentController.h"

#include <algorithm>

#include "../midi/Velocity.h"

namespace gmb {

// How long an anticipated pre-position may wait for its Note On before it is
// released. Generous enough to cover mechanical travel + a musical anticipation,
// short enough that a stray CC pair never strands a string.
static constexpr uint32_t kPrepareTimeoutUs = 2'000'000;  // 2 s

void InstrumentController::load(const Profile& p) {
    strings_.clear();
    axes_.clear();
    targets_.clear();
    active_.clear();
    chordBuffer_.clear();
    preparedFret_.clear();
    preparedId_.clear();
    preparedAtUs_.clear();
    pedalDown_ = false;

    selector_.configure(p.selector);
    selector_.setInstrument(p.instrumentView());
    selector_.reset();  // no stale CC selections carry across a profile change

    channel_ = p.midi.globalChannel;
    omni_ = p.midi.omni;
    chordWindowUs_ = static_cast<uint32_t>(p.midi.chordWindowMs) * 1000u;
    sustainEnabled_ = p.midi.sustainPedal;
    sustainCc_ = p.midi.sustainCc;
    velocityCurve_ = static_cast<int>(p.midi.velocityCurve);
    volume_ = 1.0;
    expression_ = 1.0;

    std::vector<StringSpec> specs;
    for (const auto& s : p.strings) {
        StringSpec spec;
        spec.openNote = s.openNote;
        spec.maxFret = s.maxFret;
        spec.enabled = s.enabled;
        specs.push_back(spec);

        axes_.emplace_back(s);
        StringController c;
        if (s.enabled) c.enable();  // a disabled axis stays Disabled: no notes,
                                    // even from an explicit CC selection
        strings_.push_back(c);
        targets_.push_back(StringTarget{});
        preparedFret_.push_back(-1);
        preparedId_.push_back(0);
        preparedAtUs_.push_back(0);
    }
    allocator_.setStrings(specs);
    allocator_.setStrategy(p.midi.saturationStrategy);
}

int InstrumentController::soundingCount() const {
    int n = 0;
    for (size_t i = 0; i < targets_.size(); ++i)
        // A merely-prepared string holds position but is not sounding yet.
        if (targets_[i].active && preparedId_[i] == 0) ++n;
    return n;
}

void InstrumentController::removeActiveByString(int stringIndex) {
    // A string can hold only one note; drop any stale mapping so the previous
    // note's Note Off can never stop the note that replaced it.
    for (int i = static_cast<int>(active_.size()) - 1; i >= 0; --i)
        if (active_[i].stringIndex == stringIndex) active_.erase(active_.begin() + i);
}

void InstrumentController::prepareString(int stringIndex, int fret, uint32_t nowUs) {
    if (stringIndex < 0 || stringIndex >= static_cast<int>(strings_.size())) return;
    uint32_t id = strings_[stringIndex].prepareNote(fret);
    if (id == 0) return;  // disabled / faulted axis: nothing to anticipate
    removeActiveByString(stringIndex);
    // Reserve the string so an automatic note can't grab it before its Note On.
    if (stringIndex < static_cast<int>(allocator_.runtime().size())) {
        allocator_.runtime()[stringIndex].free = false;
        allocator_.runtime()[stringIndex].currentFret = fret;
    }
    StringTarget& t = targets_[stringIndex];
    t.active = true;  // drive the carriage/finger now; the pluck stays disarmed
    t.fret = fret;
    t.positionMm = axes_[stringIndex].fretPositionMm(fret);
    t.commandId = id;
    t.velocity = 0;
    t.intensity = 0.0;
    preparedFret_[stringIndex] = fret;
    preparedId_[stringIndex] = id;
    preparedAtUs_[stringIndex] = nowUs;
}

bool InstrumentController::triggerPreparedNote(int stringIndex, int fret,
                                               uint8_t channel, uint8_t note,
                                               uint8_t velocity) {
    if (stringIndex < 0 || stringIndex >= static_cast<int>(strings_.size()))
        return false;
    if (preparedId_[stringIndex] == 0 || preparedFret_[stringIndex] != fret)
        return false;  // not prepared for this exact position -> fresh note
    if (!strings_[stringIndex].trigger(preparedId_[stringIndex])) {
        preparedFret_[stringIndex] = -1;  // stale (cancelled) prepared command
        preparedId_[stringIndex] = 0;
        return false;
    }
    // Keep the prepared move/command; only attach velocity and the Note mapping
    // so the deferred pluck fires with the right intensity.
    removeActiveByString(stringIndex);
    StringTarget& t = targets_[stringIndex];
    t.active = true;
    t.velocity = velocity;
    t.intensity = applyVelocityCurve(velocityCurve_, velocity) * attackGain();
    active_.push_back({channel, note, stringIndex, false});
    preparedFret_[stringIndex] = -1;
    preparedId_[stringIndex] = 0;
    return true;
}

void InstrumentController::startNote(int stringIndex, int fret, uint8_t channel,
                                     uint8_t note, uint8_t velocity) {
    if (stringIndex < 0 || stringIndex >= static_cast<int>(strings_.size())) return;
    removeActiveByString(stringIndex);
    preparedFret_[stringIndex] = -1;  // supersede any anticipated prepare
    preparedId_[stringIndex] = 0;
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
    t.intensity = applyVelocityCurve(velocityCurve_, velocity) * attackGain();
    active_.push_back({channel, note, stringIndex, false});
}

void InstrumentController::stopString(int stringIndex) {
    if (stringIndex < 0 || stringIndex >= static_cast<int>(strings_.size())) return;
    strings_[stringIndex].noteOff();
    targets_[stringIndex].active = false;
    preparedFret_[stringIndex] = -1;
    preparedId_[stringIndex] = 0;
    allocator_.release(stringIndex);
}

int InstrumentController::findActive(uint8_t channel, uint8_t note) const {
    for (int i = static_cast<int>(active_.size()) - 1; i >= 0; --i) {
        if (active_[i].channel == channel && active_[i].note == note) return i;
    }
    return -1;
}

void InstrumentController::handleEvent(const MidiEvent& e, uint32_t nowUs) {
    if (!accepts(e.channel)) return;  // channel / omni filter (cahier §18)

    if (e.isControlChange()) {
        if (e.data1 == 120 || e.data1 == 123) {  // all sound / notes off
            panic();
            return;
        }
        if (e.data1 == 7) {  // channel volume -> attack gain
            volume_ = e.data2 / 127.0;
            return;
        }
        if (e.data1 == 11) {  // expression -> attack gain
            expression_ = e.data2 / 127.0;
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
        // Pre-position any string whose CC selection just became complete, so the
        // matching Note On only needs to arm the pluck (prepareOnCompleteSelection).
        for (const auto& c : selector_.takeJustCompleted())
            prepareString(c.stringIndex, c.fret, nowUs);
        return;
    }

    if (e.isNoteOn()) {
        NoteResolution r = selector_.onNoteOn(e, nowUs);
        if (!r.play) return;
        if (r.source == ResolveSource::Explicit) {
            // Reuse the anticipated move if this string was prepared for this fret;
            // otherwise start a fresh note.
            if (!triggerPreparedNote(r.stringIndex, r.fret, e.channel, e.data1,
                                     e.data2))
                startNote(r.stringIndex, r.fret, e.channel, e.data1, e.data2);
        } else {
            // Automatic allocation is deferred to group chord notes (§17.2).
            chordBuffer_.push_back({e.channel, e.data1, e.data2, nowUs});
            if (chordWindowUs_ == 0) flushChord();
        }
        return;
    }

    if (e.isNoteOff()) {
        // Always clear the selector's record for this note so its history never
        // keeps stale instances (sustain/selector sync).
        ActiveNote a;
        selector_.onNoteOff(e, &a);

        // Cancel a note still waiting in the chord buffer (ghost-note fix): the
        // Note Off arrived before the grouping window flushed.
        for (int i = static_cast<int>(chordBuffer_.size()) - 1; i >= 0; --i) {
            if (chordBuffer_[i].channel == e.channel &&
                chordBuffer_[i].note == e.data1) {
                chordBuffer_.erase(chordBuffer_.begin() + i);
                return;
            }
        }

        int idx = findActive(e.channel, e.data1);
        if (idx < 0) return;
        if (pedalDown_ && sustainEnabled_) {
            active_[idx].heldByPedal = true;  // keep sounding until pedal up
            return;
        }
        int stringIndex = active_[idx].stringIndex;
        active_.erase(active_.begin() + idx);
        stopString(stringIndex);
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
        removeActiveByString(a.stringIndex);
        uint32_t id = strings_[a.stringIndex].noteOn(a.fret);
        if (id == 0) continue;
        StringTarget& t = targets_[a.stringIndex];
        t.active = true;
        t.fret = a.fret;
        t.positionMm = axes_[a.stringIndex].fretPositionMm(a.fret);
        t.commandId = id;
        t.velocity = src.velocity;
        t.intensity = applyVelocityCurve(velocityCurve_, src.velocity) * attackGain();
        active_.push_back({src.channel, src.note, a.stringIndex, false});
    }
    chordBuffer_.clear();
}

void InstrumentController::tick(uint32_t nowUs) {
    // Expire anticipated preparations that never received their Note On, so a
    // string is never reserved (finger down, carriage held, allocator busy)
    // indefinitely. The window is independent of the CC-pairing timeout because a
    // pre-position also has to travel mechanically before its note can arrive.
    for (size_t i = 0; i < preparedId_.size(); ++i) {
        if (preparedId_[i] != 0 &&
            static_cast<int32_t>(nowUs - preparedAtUs_[i]) >=
                static_cast<int32_t>(kPrepareTimeoutUs)) {
            stopString(static_cast<int>(i));  // lift finger, free allocator, drop target
        }
    }
    if (chordBuffer_.empty()) return;
    if (nowUs - chordBuffer_.front().atUs >= chordWindowUs_) flushChord();
}

void InstrumentController::faultString(size_t index) {
    if (index >= strings_.size()) return;
    strings_[index].fault();                          // -> Fault: noteOn() refused
    allocator_.setFaulted(static_cast<int>(index), true);
    allocator_.release(static_cast<int>(index));
    targets_[index].active = false;
    preparedFret_[index] = -1;
    preparedId_[index] = 0;
    removeActiveByString(static_cast<int>(index));    // drop any active note here
}

void InstrumentController::panic() {
    for (auto& s : strings_) s.panic();
    for (auto& t : targets_) t.active = false;
    for (size_t i = 0; i < strings_.size(); ++i) allocator_.release(static_cast<int>(i));
    std::fill(preparedFret_.begin(), preparedFret_.end(), -1);
    std::fill(preparedId_.begin(), preparedId_.end(), 0u);
    active_.clear();
    chordBuffer_.clear();
    pedalDown_ = false;
    volume_ = 1.0;
    expression_ = 1.0;
    selector_.reset();  // clear pending/active CC selections on panic
}

}  // namespace gmb
