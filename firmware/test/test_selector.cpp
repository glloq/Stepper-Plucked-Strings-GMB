#include "TestFramework.h"
#include "../src/core/midi/StringFretSelector.h"

using namespace gmb;

// A standard 4-string ukulele-ish view: open notes, 12 frets each.
static InstrumentView makeView4() {
    InstrumentView v;
    v.stringCount = 4;
    v.openNotes = {67, 60, 64, 69};  // GCEA
    v.maxFretPerString = {12, 12, 12, 12};
    return v;
}

static MidiEvent cc(uint8_t ch, uint8_t num, uint8_t val, uint32_t t) {
    MidiEvent e;
    e.type = static_cast<uint8_t>(MidiType::ControlChange);
    e.channel = ch;
    e.data1 = num;
    e.data2 = val;
    e.timestampUs = t;
    return e;
}
static MidiEvent noteOn(uint8_t ch, uint8_t note, uint8_t vel, uint32_t t) {
    MidiEvent e;
    e.type = static_cast<uint8_t>(MidiType::NoteOn);
    e.channel = ch;
    e.data1 = note;
    e.data2 = vel;
    e.timestampUs = t;
    return e;
}

// Acceptance criteria 1, 2 (selection spec 19): CC20/CC21 default selectors.
TEST(explicit_cc_selects_string_and_fret) {
    StringFretSelector sel;
    SelectorConfig cfg;
    cfg.mode = SelectionMode::Explicit;
    cfg.string.maximum = 4;
    cfg.fret.maximum = 12;
    sel.configure(cfg);
    sel.setInstrument(makeView4());

    sel.onControlChange(cc(0, 20, 3, 0));  // string 3 (index 2)
    sel.onControlChange(cc(0, 21, 5, 1));  // fret 5
    // Note that matches string index 2 (open 64) + fret 5 = 69.
    NoteResolution r = sel.onNoteOn(noteOn(0, 69, 100, 2), 3);
    CHECK(r.play);
    CHECK(r.source == ResolveSource::Explicit);
    CHECK_EQ((int)r.stringIndex, 2);
    CHECK_EQ((int)r.fret, 5);
}

// LastValid is per-channel: a valid selection on channel 0 must not be reused as
// the LastValid fallback for channel 1 (audit P1-5).
TEST(last_valid_is_per_channel) {
    StringFretSelector sel;
    SelectorConfig cfg;
    cfg.mode = SelectionMode::Explicit;
    cfg.perMidiChannel = true;
    cfg.string.maximum = 4;
    cfg.fret.maximum = 12;
    cfg.missingSelectionPolicy = InvalidValuePolicy::LastValid;
    sel.configure(cfg);
    sel.setInstrument(makeView4());

    // Channel 0 makes a full valid selection -> sets channel 0's LastValid.
    sel.onControlChange(cc(0, 20, 3, 0));
    sel.onControlChange(cc(0, 21, 5, 1));
    NoteResolution r0 = sel.onNoteOn(noteOn(0, 69, 100, 2), 3);
    CHECK(r0.source == ResolveSource::Explicit);

    // Channel 1 has NO selection: LastValid must not borrow channel 0's pair, so
    // it falls back to automatic instead of an explicit channel-0 assignment.
    NoteResolution r1 = sel.onNoteOn(noteOn(1, 62, 100, 10), 11);
    CHECK(r1.source == ResolveSource::Automatic);
}

// Acceptance criteria 11, 12: chord selections stay in a FIFO, not last-wins.
TEST(chord_selections_are_fifo) {
    StringFretSelector sel;
    SelectorConfig cfg;
    cfg.mode = SelectionMode::Explicit;
    cfg.string.maximum = 4;
    cfg.fret.maximum = 12;
    sel.configure(cfg);
    sel.setInstrument(makeView4());

    sel.onControlChange(cc(0, 20, 1, 0));
    sel.onControlChange(cc(0, 20, 3, 0));
    sel.onControlChange(cc(0, 20, 4, 0));
    sel.onControlChange(cc(0, 21, 2, 0));
    sel.onControlChange(cc(0, 21, 5, 0));
    sel.onControlChange(cc(0, 21, 7, 0));

    // Selections: (1,2)->idx0, (3,5)->idx2, (4,7)->idx3.
    NoteResolution r1 = sel.onNoteOn(noteOn(0, 69, 100, 0), 0);  // 67+2
    NoteResolution r2 = sel.onNoteOn(noteOn(0, 69, 100, 0), 0);  // 64+5
    NoteResolution r3 = sel.onNoteOn(noteOn(0, 76, 100, 0), 0);  // 69+7
    CHECK_EQ((int)r1.stringIndex, 0);
    CHECK_EQ((int)r1.fret, 2);
    CHECK_EQ((int)r2.stringIndex, 2);
    CHECK_EQ((int)r2.fret, 5);
    CHECK_EQ((int)r3.stringIndex, 3);
    CHECK_EQ((int)r3.fret, 7);
}

// Acceptance criterion 10: expired selections are dropped.
TEST(expired_selection_is_dropped) {
    StringFretSelector sel;
    SelectorConfig cfg;
    cfg.mode = SelectionMode::Hybrid;
    cfg.selectionTimeoutMs = 100;  // 100 ms
    cfg.string.maximum = 4;
    cfg.fret.maximum = 12;
    sel.configure(cfg);
    sel.setInstrument(makeView4());

    sel.onControlChange(cc(0, 20, 3, 0));
    sel.onControlChange(cc(0, 21, 5, 0));
    // Note On arrives 200 ms later -> selection expired -> hybrid fallback.
    NoteResolution r = sel.onNoteOn(noteOn(0, 69, 100, 200000), 200000);
    CHECK(r.play);
    CHECK(r.source == ResolveSource::Automatic);
}

// Acceptance criterion 8: hybrid falls back to automatic when no CC selection.
TEST(hybrid_falls_back_to_automatic) {
    StringFretSelector sel;
    SelectorConfig cfg;
    cfg.mode = SelectionMode::Hybrid;
    sel.configure(cfg);
    sel.setInstrument(makeView4());
    NoteResolution r = sel.onNoteOn(noteOn(0, 64, 100, 0), 0);
    CHECK(r.play);
    CHECK(r.source == ResolveSource::Automatic);
}

// Acceptance criterion 5: reversed string order.
TEST(reversed_string_order) {
    StringFretSelector sel;
    SelectorConfig cfg;
    cfg.mode = SelectionMode::Explicit;
    cfg.string.maximum = 4;
    cfg.string.reverseOrder = true;
    cfg.fret.maximum = 12;
    sel.configure(cfg);
    sel.setInstrument(makeView4());
    // CC value 1 -> physical axis 3 when reversed.
    CHECK_EQ(sel.mapStringValue(1), 3);
    CHECK_EQ(sel.mapStringValue(4), 0);
}

// Acceptance criterion 6: custom mapping table.
TEST(custom_string_mapping) {
    StringFretSelector sel;
    SelectorConfig cfg;
    cfg.mode = SelectionMode::Explicit;
    cfg.string.maximum = 4;
    cfg.string.mapping = {2, 0, 3, 1};  // logical -> axis
    cfg.fret.maximum = 12;
    sel.configure(cfg);
    sel.setInstrument(makeView4());
    CHECK_EQ(sel.mapStringValue(1), 2);
    CHECK_EQ(sel.mapStringValue(2), 0);
    CHECK_EQ(sel.mapStringValue(3), 3);
    CHECK_EQ(sel.mapStringValue(4), 1);
}

// Acceptance criterion 15: Note Off releases the actually-used string.
TEST(note_off_recovers_assignment) {
    StringFretSelector sel;
    SelectorConfig cfg;
    cfg.mode = SelectionMode::Explicit;
    cfg.string.maximum = 4;
    cfg.fret.maximum = 12;
    sel.configure(cfg);
    sel.setInstrument(makeView4());

    sel.onControlChange(cc(0, 20, 3, 0));
    sel.onControlChange(cc(0, 21, 5, 0));
    NoteResolution r = sel.onNoteOn(noteOn(0, 69, 100, 0), 0);
    CHECK(r.play);

    MidiEvent off = noteOn(0, 69, 0, 0);  // NoteOn vel 0 == NoteOff
    off.type = static_cast<uint8_t>(MidiType::NoteOff);
    ActiveNote a;
    CHECK(sel.onNoteOff(off, &a));
    CHECK_EQ((int)a.stringIndex, 2);
    CHECK_EQ((int)a.fret, 5);
}

// Per-channel selection isolation (selection spec section 8/9).
TEST(selections_are_per_channel) {
    StringFretSelector sel;
    SelectorConfig cfg;
    cfg.mode = SelectionMode::Explicit;
    cfg.perMidiChannel = true;
    cfg.string.maximum = 4;
    cfg.fret.maximum = 12;
    sel.configure(cfg);
    sel.setInstrument(makeView4());

    sel.onControlChange(cc(0, 20, 1, 0));
    sel.onControlChange(cc(1, 20, 4, 0));
    sel.onControlChange(cc(0, 21, 2, 0));
    sel.onControlChange(cc(1, 21, 7, 0));

    NoteResolution r0 = sel.onNoteOn(noteOn(0, 69, 100, 0), 0);
    NoteResolution r1 = sel.onNoteOn(noteOn(1, 76, 100, 0), 0);
    CHECK_EQ((int)r0.stringIndex, 0);
    CHECK_EQ((int)r0.fret, 2);
    CHECK_EQ((int)r1.stringIndex, 3);
    CHECK_EQ((int)r1.fret, 7);
}

// GMB preset adapts ranges to the active instrument (selection spec section 3).
TEST(gmb_preset_adapts_ranges) {
    StringFretSelector sel;
    sel.setInstrument(makeView4());
    sel.applyGmbPreset();
    CHECK_EQ((int)sel.config().string.ccNumber, 20);
    CHECK_EQ((int)sel.config().fret.ccNumber, 21);
    CHECK_EQ((int)sel.config().string.maximum, 4);
    CHECK_EQ((int)sel.config().fret.maximum, 12);
    CHECK(sel.config().mode == SelectionMode::Hybrid);
}
