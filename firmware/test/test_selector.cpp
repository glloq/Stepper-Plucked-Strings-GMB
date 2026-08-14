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

// ---------------------------------------------------------------------------
// Regressions in the CC selection path. Each of these makes the instrument play
// the WRONG note (or a note it should have refused) rather than fail loudly, so
// they are the kind that survive a bench session unnoticed.
// ---------------------------------------------------------------------------

// The offset is applied BEFORE the range check (STRING_FRET_SELECTION.md:
// "logical = CC + offset, then validated"). Validating the raw value instead
// shifts the whole accepted band by the offset, so a configured offset silently
// accepts the wrong CC values and rejects the right ones.
TEST(offset_is_applied_before_the_range_check) {
    StringFretSelector sel;
    SelectorConfig cfg;
    cfg.mode = SelectionMode::Explicit;
    cfg.string.minimum = 1;
    cfg.string.maximum = 4;
    cfg.string.offset = 1;   // a controller that numbers its strings from 0
    cfg.fret.maximum = 12;
    cfg.fret.invalidValuePolicy = InvalidValuePolicy::Reject;  // observe refusals
    sel.configure(cfg);
    sel.setInstrument(makeView4());

    // Raw 0 + offset 1 = logical 1 -> string 1 (axis 0). Raw 0 is below the raw
    // minimum, so the buggy order rejected exactly the value the offset exists for.
    sel.onControlChange(cc(0, 20, 0, 1000));
    sel.onControlChange(cc(0, 21, 3, 1100));
    NoteResolution r = sel.onNoteOn(noteOn(0, 70, 100, 1200), 1300);
    CHECK(r.play);
    CHECK_EQ((int)r.stringIndex, 0);
    CHECK_EQ((int)r.fret, 3);

    // And the top of the band moves with it: raw 4 + 1 = 5 is now out of range.
    sel.onControlChange(cc(0, 20, 4, 2000));
    sel.onControlChange(cc(0, 21, 3, 2100));
    NoteResolution r2 = sel.onNoteOn(noteOn(0, 70, 100, 2200), 2300);
    CHECK(!r2.play);
    CHECK(r2.source == ResolveSource::Rejected);
}

// An out-of-range CC value must FILL its slot (as invalid), not vanish. If it
// vanishes, the half-selection left behind is completed by the next unrelated CC
// and the note is played on a string nobody asked for.
TEST(an_invalid_cc_value_cannot_be_completed_by_a_later_one) {
    StringFretSelector sel;
    SelectorConfig cfg;
    cfg.mode = SelectionMode::Explicit;
    cfg.string.minimum = 1;
    cfg.string.maximum = 4;
    cfg.fret.maximum = 12;
    cfg.fret.invalidValuePolicy = InvalidValuePolicy::Reject;  // observe refusals
    sel.configure(cfg);
    sel.setInstrument(makeView4());

    sel.onControlChange(cc(0, 21, 5, 1000));   // fret 5, waiting for its string
    sel.onControlChange(cc(0, 20, 9, 1100));   // string 9: out of range
    // The fret is now bound to the INVALID string, so this later valid string CC
    // opens a NEW selection instead of adopting the orphaned fret.
    sel.onControlChange(cc(0, 20, 2, 1200));

    // First note: the invalid pair is the oldest complete selection, and it is
    // refused rather than played on some arbitrary string.
    NoteResolution r = sel.onNoteOn(noteOn(0, 65, 100, 1300), 1400);
    CHECK(!r.play);
    CHECK(r.source == ResolveSource::Rejected);

    // The later valid string CC is still waiting for a fret of its own — it did
    // NOT silently inherit fret 5 from the abandoned selection.
    sel.onControlChange(cc(0, 21, 9, 1500));
    NoteResolution r2 = sel.onNoteOn(noteOn(0, 69, 100, 1600), 1700);
    CHECK(r2.play);
    CHECK_EQ((int)r2.stringIndex, 1);  // string 2 -> axis 1
    CHECK_EQ((int)r2.fret, 9);         // the fret it was actually given
}

// An expired selection must not shadow a newer, still-valid one queued behind it:
// taking the first complete entry regardless rejects a perfectly good note
// because an older one timed out.
TEST(an_expired_selection_does_not_shadow_a_valid_one) {
    StringFretSelector sel;
    SelectorConfig cfg;
    cfg.mode = SelectionMode::Explicit;
    cfg.string.maximum = 4;
    cfg.fret.maximum = 12;
    cfg.selectionTimeoutMs = 100;   // 100 ms
    sel.configure(cfg);
    sel.setInstrument(makeView4());

    // Old selection at t=0 (expires at 100 ms), fresh one at t=90 ms.
    sel.onControlChange(cc(0, 20, 1, 0));
    sel.onControlChange(cc(0, 21, 2, 0));
    sel.onControlChange(cc(0, 20, 3, 90000));
    sel.onControlChange(cc(0, 21, 7, 90000));

    // At t=150 ms the first has expired, the second has not.
    NoteResolution r = sel.onNoteOn(noteOn(0, 71, 100, 150000), 150000);
    CHECK(r.play);
    CHECK_EQ((int)r.stringIndex, 2);  // string 3 -> axis 2
    CHECK_EQ((int)r.fret, 7);
}
