#include "TestFramework.h"
#include "../src/core/configuration/Profile.h"
#include "../src/core/gmb/GmbSysExService.h"
#include "../src/core/instrument/InstrumentController.h"

using namespace gmb;

static Profile ukulele() {
    Profile p = Profile::makeDefault("Ukulele", 4, {67, 60, 64, 69}, 12);
    p.selector.mode = SelectionMode::Hybrid;
    p.selector.string.maximum = 4;
    p.selector.fret.maximum = 12;
    return p;
}

static MidiEvent cc(uint8_t ch, uint8_t n, uint8_t v) {
    MidiEvent e;
    e.type = (uint8_t)MidiType::ControlChange;
    e.channel = ch; e.data1 = n; e.data2 = v;
    return e;
}
static MidiEvent noteOn(uint8_t ch, uint8_t note, uint8_t vel) {
    MidiEvent e;
    e.type = (uint8_t)MidiType::NoteOn;
    e.channel = ch; e.data1 = note; e.data2 = vel;
    return e;
}
static MidiEvent noteOff(uint8_t ch, uint8_t note) {
    MidiEvent e;
    e.type = (uint8_t)MidiType::NoteOff;
    e.channel = ch; e.data1 = note;
    return e;
}

// Automatic mode: a bare Note On is grouped, then allocated on flush.
TEST(controller_automatic_note) {
    InstrumentController ic;
    ic.load(ukulele());
    ic.handleEvent(noteOn(0, 62, 100), 0);  // D4: playable on C string fret 2
    ic.tick(5000);                          // past the 3 ms chord window
    CHECK_EQ(ic.soundingCount(), 1);
    ic.handleEvent(noteOff(0, 62), 6000);
    CHECK_EQ(ic.soundingCount(), 0);
}

// Notes arriving inside the chord window are allocated together via allocateChord.
TEST(controller_chord_grouping_window) {
    InstrumentController ic;
    ic.load(ukulele());
    ic.handleEvent(noteOn(0, 67, 100), 0);
    ic.handleEvent(noteOn(0, 60, 100), 500);   // +0.5 ms
    ic.handleEvent(noteOn(0, 64, 100), 1000);  // +1 ms
    CHECK_EQ(ic.soundingCount(), 0);           // still buffered
    ic.tick(2000);                             // window not elapsed yet
    CHECK_EQ(ic.soundingCount(), 0);
    ic.tick(4000);                             // > 3 ms since first note
    CHECK_EQ(ic.soundingCount(), 3);
}

// Channel filter: events on other channels are ignored unless omni.
TEST(controller_channel_filter) {
    Profile p = ukulele();
    p.midi.globalChannel = 0;
    p.midi.omni = false;
    InstrumentController ic;
    ic.load(p);
    ic.handleEvent(noteOn(1, 62, 100), 0);  // wrong channel
    ic.tick(5000);
    CHECK_EQ(ic.soundingCount(), 0);
    ic.handleEvent(noteOn(0, 62, 100), 6000);
    ic.tick(11000);
    CHECK_EQ(ic.soundingCount(), 1);
}

// Sustain pedal (CC64) holds a note through its Note Off until the pedal lifts.
TEST(controller_sustain_pedal) {
    Profile p = ukulele();
    p.midi.sustainPedal = true;
    InstrumentController ic;
    ic.load(p);
    ic.handleEvent(cc(0, 64, 127), 0);      // pedal down
    ic.handleEvent(noteOn(0, 62, 100), 100);
    ic.tick(5000);
    CHECK_EQ(ic.soundingCount(), 1);
    ic.handleEvent(noteOff(0, 62), 6000);   // held by pedal
    CHECK_EQ(ic.soundingCount(), 1);
    ic.handleEvent(cc(0, 64, 0), 7000);     // pedal up -> release
    CHECK_EQ(ic.soundingCount(), 0);
}

// Explicit CC selection routes to the exact string/fret.
TEST(controller_explicit_selection) {
    Profile p = ukulele();
    p.selector.mode = SelectionMode::Explicit;
    InstrumentController ic;
    ic.load(p);
    ic.handleEvent(cc(0, 20, 3), 0);   // string 3 -> index 2 (open 64)
    ic.handleEvent(cc(0, 21, 5), 0);   // fret 5
    ic.handleEvent(noteOn(0, 69, 100), 0);  // 64+5 = 69
    CHECK(ic.target(2).active);
    CHECK_EQ(ic.target(2).fret, 5);
}

// A 4-note chord uses four distinct strings (criterion 14 at ukulele scale).
TEST(controller_chord) {
    InstrumentController ic;
    ic.load(ukulele());
    ic.handleEvent(noteOn(0, 67, 100), 0);
    ic.handleEvent(noteOn(0, 60, 100), 0);
    ic.handleEvent(noteOn(0, 64, 100), 0);
    ic.handleEvent(noteOn(0, 69, 100), 0);
    ic.tick(5000);
    CHECK_EQ(ic.soundingCount(), 4);
}

// Panic (CC120) neutralises everything (criterion 16).
TEST(controller_panic_clears) {
    InstrumentController ic;
    ic.load(ukulele());
    ic.handleEvent(noteOn(0, 67, 100), 0);
    ic.handleEvent(noteOn(0, 60, 100), 0);
    ic.tick(5000);
    CHECK(ic.soundingCount() > 0);
    ic.handleEvent(cc(0, 120, 0), 6000);  // all sound off
    CHECK_EQ(ic.soundingCount(), 0);
}

// SysEx service answers a full discovery from one snapshot (criteria 1,8,9,10).
TEST(sysex_service_discovery) {
    Profile p = ukulele();
    GmbSysExService svc;
    svc.rebuild(p);

    uint8_t identityReq[] = {0xF0, 0x7D, 0x00, 0x01, 0x00, 0xF7};
    auto id = svc.handleMessage(identityReq, sizeof(identityReq), 10);
    CHECK(!id.empty());
    CHECK_EQ((int)id[3], 0x01);

    uint8_t capsReq[] = {0xF0, 0x7D, 0x00, 0x06, 0x00, 0x00, 0xF7};
    auto caps = svc.handleMessage(capsReq, sizeof(capsReq), 11);
    CHECK(!caps.empty());
    CHECK_EQ((int)caps[3], 0x06);

    uint8_t strReq[] = {0xF0, 0x7D, 0x00, 0x07, 0x00, 0x00, 0xF7};
    auto str = svc.handleMessage(strReq, sizeof(strReq), 12);
    CHECK(!str.empty());
    CHECK_EQ((int)str[3], 0x07);
}

// Foreign channel is rejected (SysEx spec §20).
TEST(sysex_service_rejects_foreign_channel) {
    Profile p = ukulele();
    p.midi.globalChannel = 0;
    GmbSysExService svc;
    svc.rebuild(p);
    uint8_t req[] = {0xF0, 0x7D, 0x00, 0x06, 0x00, 0x05, 0xF7};  // channel 5
    auto r = svc.handleMessage(req, sizeof(req), 10);
    CHECK(r.empty());
}

// The SysEx service rate-limits a flood of requests (token bucket).
TEST(sysex_service_rate_limits_flood) {
    Profile p = ukulele();
    GmbSysExService svc;
    svc.rebuild(p);
    uint8_t req[] = {0xF0, 0x7D, 0x00, 0x06, 0x00, 0x00, 0xF7};
    int answered = 0;
    for (int i = 0; i < 40; ++i)  // all at the same instant
        if (!svc.handleMessage(req, sizeof(req), 100).empty()) ++answered;
    CHECK(answered <= 16);        // burst capped
    CHECK(answered > 0);
    // After enough time the bucket refills and responses resume.
    CHECK(!svc.handleMessage(req, sizeof(req), 100 + 200).empty());
}

// A config change increments the revision and the notification carries it.
TEST(sysex_service_notification_after_change) {
    Profile p = ukulele();
    GmbSysExService svc;
    svc.rebuild(p);
    p.capabilitiesRevision = 2;  // config edited & saved
    svc.rebuild(p);
    auto note = svc.notification(kStringConfigChanged);
    CHECK_EQ((int)note[3], 0x08);
    CHECK(GmbSysEx::isWellFormed(note.data(), note.size()));
}
