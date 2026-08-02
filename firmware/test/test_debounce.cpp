#include "TestFramework.h"
#include "../src/core/midi/StringFretSelector.h"
#include "../src/core/util/Debounce.h"

using namespace gmb;

TEST(debounce_requires_stable_window) {
    Debouncer d;
    d.configure(/*stableMs=*/5, /*initial=*/false);
    d.update(0, false);          // seed
    CHECK(!d.state());
    // A short spike shorter than the window is rejected.
    d.update(1, true);
    d.update(2, false);          // bounced back before 5 ms
    CHECK(!d.state());
    // A level held past the window is accepted.
    d.update(10, true);
    CHECK(!d.state());           // just started, not yet 5 ms
    CHECK(d.update(16, true));   // 6 ms held -> accepted
    // And released only after another stable window.
    d.update(17, false);
    CHECK(d.state());            // still high, window not elapsed
    CHECK(!d.update(30, false)); // held low -> low
}

TEST(debounce_ignores_single_spike) {
    Debouncer d;
    d.configure(10, false);
    d.update(0, false);
    for (int t = 1; t < 100; ++t) {
        bool raw = (t == 50);    // one-tick spike
        CHECK(!d.update(t, raw));
    }
}

// --- selector: expiredSelectionPolicy applied distinctly ---
static MidiEvent cc(uint8_t ch, uint8_t n, uint8_t v, uint32_t t) {
    MidiEvent e; e.type = (uint8_t)MidiType::ControlChange;
    e.channel = ch; e.data1 = n; e.data2 = v; e.timestampUs = t; return e;
}
static MidiEvent noteOn(uint8_t ch, uint8_t note, uint8_t vel, uint32_t t) {
    MidiEvent e; e.type = (uint8_t)MidiType::NoteOn;
    e.channel = ch; e.data1 = note; e.data2 = vel; e.timestampUs = t; return e;
}
static InstrumentView view4() {
    InstrumentView v; v.stringCount = 4; v.openNotes = {67, 60, 64, 69};
    v.maxFretPerString = {12, 12, 12, 12}; return v;
}

TEST(expired_selection_reject_policy) {
    StringFretSelector sel;
    SelectorConfig cfg;
    cfg.mode = SelectionMode::Explicit;   // not hybrid, so policy is honoured
    cfg.selectionTimeoutMs = 100;
    cfg.expiredSelectionPolicy = InvalidValuePolicy::Reject;
    cfg.string.maximum = 4; cfg.fret.maximum = 12;
    sel.configure(cfg);
    sel.setInstrument(view4());

    sel.onControlChange(cc(0, 20, 3, 0));
    sel.onControlChange(cc(0, 21, 5, 0));
    // Note On 200 ms later -> the complete selection has expired.
    NoteResolution r = sel.onNoteOn(noteOn(0, 69, 100, 200000), 200000);
    CHECK(!r.play);
    CHECK(r.source == ResolveSource::Rejected);
}

TEST(expired_selection_hybrid_falls_back) {
    StringFretSelector sel;
    SelectorConfig cfg;
    cfg.mode = SelectionMode::Hybrid;
    cfg.selectionTimeoutMs = 100;
    cfg.string.maximum = 4; cfg.fret.maximum = 12;
    sel.configure(cfg);
    sel.setInstrument(view4());
    sel.onControlChange(cc(0, 20, 3, 0));
    sel.onControlChange(cc(0, 21, 5, 0));
    NoteResolution r = sel.onNoteOn(noteOn(0, 69, 100, 200000), 200000);
    CHECK(r.play);
    CHECK(r.source == ResolveSource::Automatic);
}
