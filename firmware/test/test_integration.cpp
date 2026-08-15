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

// Auto allocation must honour the capo: with capo +2 the open A string (69)
// sounds at MIDI 71, so note 71 is played OPEN (fret 0), not fret 2.
TEST(controller_auto_capo_shifts_open) {
    Profile p = ukulele();  // strings {67,60,64,69}
    p.instrument.capo = 2;
    InstrumentController ic;
    ic.load(p);
    ic.handleEvent(noteOn(0, 71, 100), 0);  // 69 + capo 2
    ic.tick(5000);
    CHECK_EQ(ic.soundingCount(), 1);
    // Find the string that took the note; it must be open (fret 0), the A string.
    int played = -1;
    for (size_t i = 0; i < ic.stringCount(); ++i)
        if (ic.target(i).active) played = static_cast<int>(i);
    CHECK(played >= 0);
    CHECK_EQ(ic.target(played).fret, 0);
}

// Auto allocation must honour MIDI transpose: with transpose -12 a note one
// octave higher lands on the same fret it would without transpose.
TEST(controller_auto_transpose) {
    Profile p = ukulele();
    p.midi.transpose = -12;
    InstrumentController ic;
    ic.load(p);
    // Effective open C string = 60 - 12 = 48, so MIDI 50 sounds at fret 2 (and
    // is out of range on the other strings) — proves the transpose is applied.
    ic.handleEvent(noteOn(0, 50, 100), 0);
    ic.tick(5000);
    CHECK_EQ(ic.soundingCount(), 1);
    int played = -1;
    for (size_t i = 0; i < ic.stringCount(); ++i)
        if (ic.target(i).active) played = static_cast<int>(i);
    CHECK(played >= 0);
    CHECK_EQ(ic.target(played).fret, 2);
}

// A transposed open note that falls BELOW 0 must stay signed: clamping it to 0
// would place a note at the wrong fret. Open note 1, transpose -3 => effective
// open -2, so MIDI 0 plays at fret 2 (not fret 0).
TEST(controller_negative_effective_open_note) {
    Profile p = Profile::makeDefault("Low", 1, {1}, 12);
    p.instrument.transpose = -3;
    InstrumentController ic;
    ic.load(p);
    ic.handleEvent(noteOn(0, 0, 100), 0);
    ic.tick(5000);
    CHECK_EQ(ic.soundingCount(), 1);
    CHECK_EQ(ic.target(0).fret, 2);
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

// prepareOnCompleteSelection: a complete CC selection pre-positions the string
// (target active, not yet sounding); the Note On reuses that move and only fires.
TEST(controller_prepare_on_complete_selection) {
    Profile p = ukulele();
    p.selector.mode = SelectionMode::Explicit;
    p.selector.prepareOnCompleteSelection = true;
    InstrumentController ic;
    ic.load(p);
    ic.handleEvent(cc(0, 20, 3), 0);   // string 3 -> index 2 (open 64)
    ic.handleEvent(cc(0, 21, 5), 0);   // fret 5 completes the selection
    CHECK(ic.target(2).active);        // pre-positioned in anticipation
    CHECK_EQ(ic.target(2).fret, 5);
    CHECK_EQ(ic.soundingCount(), 0);   // prepared, not sounding yet
    uint32_t prepId = ic.target(2).commandId;
    ic.handleEvent(noteOn(0, 69, 110), 100);   // 64+5 = 69
    CHECK_EQ(ic.soundingCount(), 1);           // now sounding
    CHECK_EQ(ic.target(2).commandId, prepId);  // same move reused, no re-position
    CHECK_EQ(ic.target(2).velocity, 110);      // velocity attached at trigger time
}

// A complete CC selection that never gets its Note On must not reserve the string
// past the SELECTION's own expiry (audit P1-6): tick() releases it in step with
// the selection, freeing the string for a later automatic note.
TEST(controller_prepare_expires_with_selection) {
    Profile p = ukulele();
    p.selector.mode = SelectionMode::Explicit;
    p.selector.prepareOnCompleteSelection = true;
    p.selector.selectionTimeoutMs = 100;  // expiry at 100 ms (CC timestamp = 0)
    InstrumentController ic;
    ic.load(p);
    ic.handleEvent(cc(0, 20, 3), 0);   // string 3 -> index 2
    ic.handleEvent(cc(0, 21, 5), 0);   // completes -> pre-positioned
    CHECK(ic.target(2).active);
    ic.tick(50'000);                   // 50 ms: within the selection window
    CHECK(ic.target(2).active);
    ic.tick(150'000);                  // 150 ms: selection expired -> released
    CHECK(!ic.target(2).active);
    // The string is free again for a fresh automatic note.
    ic.handleEvent(noteOn(0, 62, 100), 200'000);
    ic.tick(210'000);
    CHECK_EQ(ic.soundingCount(), 1);
}

// With prepareOnCompleteSelection off, no pre-positioning happens before Note On.
TEST(controller_prepare_disabled) {
    Profile p = ukulele();
    p.selector.mode = SelectionMode::Explicit;
    p.selector.prepareOnCompleteSelection = false;
    InstrumentController ic;
    ic.load(p);
    ic.handleEvent(cc(0, 20, 3), 0);
    ic.handleEvent(cc(0, 21, 5), 0);
    CHECK(!ic.target(2).active);       // nothing moves until the Note On
    ic.handleEvent(noteOn(0, 69, 100), 100);
    CHECK(ic.target(2).active);
    CHECK_EQ(ic.soundingCount(), 1);
}

// CC7 (volume) and CC11 (expression) scale the attack intensity of later plucks.
TEST(controller_cc7_cc11_scale_attack) {
    Profile p = ukulele();
    p.selector.mode = SelectionMode::Explicit;
    p.selector.prepareOnCompleteSelection = false;
    InstrumentController ic;
    ic.load(p);
    ic.handleEvent(cc(0, 20, 3), 0);   // string index 2
    ic.handleEvent(cc(0, 21, 5), 0);   // fret 5
    ic.handleEvent(noteOn(0, 69, 100), 0);
    double full = ic.target(2).intensity;
    CHECK(full > 0.0);
    ic.handleEvent(noteOff(0, 69), 100);
    // Halve the volume: the next pluck should attack softer.
    ic.handleEvent(cc(0, 7, 64), 200);
    ic.handleEvent(cc(0, 20, 3), 300);
    ic.handleEvent(cc(0, 21, 5), 300);
    ic.handleEvent(noteOn(0, 69, 100), 300);
    double half = ic.target(2).intensity;
    CHECK(half > 0.0);
    CHECK(half < full);
}

// An explicit CC selection that resolves to a FAULTED string must fall back to
// automatic allocation, not drop the note.
TEST(controller_explicit_fallback_on_faulted_string) {
    Profile p = ukulele();
    p.selector.mode = SelectionMode::Explicit;
    p.selector.prepareOnCompleteSelection = false;
    InstrumentController ic;
    ic.load(p);
    ic.faultString(2);                       // string index 2 out of service
    ic.handleEvent(cc(0, 20, 3), 0);         // explicitly select string 3 -> index 2
    ic.handleEvent(cc(0, 21, 5), 0);         // fret 5 -> note 69
    ic.handleEvent(noteOn(0, 69, 100), 0);
    ic.tick(5000);                           // flush the automatic fallback
    CHECK_EQ(ic.soundingCount(), 1);         // played on a working string, not dropped
    CHECK(!ic.target(2).active);             // never on the faulted string
}

// Explicit chord notes each play on their OWN string — strumming is per string
// (there is no shared strummer / strum group).
TEST(controller_explicit_chord_per_string) {
    Profile p = ukulele();
    p.selector.mode = SelectionMode::Explicit;
    p.selector.prepareOnCompleteSelection = false;
    p.midi.chordWindowMs = 5;  // 5 ms grouping window
    InstrumentController ic;
    ic.load(p);
    ic.handleEvent(cc(0, 20, 1), 0); ic.handleEvent(cc(0, 21, 0), 0);
    ic.handleEvent(noteOn(0, 67, 100), 0);        // string index 0
    ic.handleEvent(cc(0, 20, 2), 1000); ic.handleEvent(cc(0, 21, 0), 1000);
    ic.handleEvent(noteOn(0, 60, 100), 1000);     // string index 1, +1 ms
    CHECK(ic.target(0).active);
    CHECK(ic.target(1).active);
    CHECK(!ic.target(2).active);
    ic.handleEvent(cc(0, 20, 3), 10000); ic.handleEvent(cc(0, 21, 0), 10000);
    ic.handleEvent(noteOn(0, 64, 100), 10000);    // string index 2, +10 ms
    CHECK(ic.target(2).active);
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
    // GMB v2 moved the spontaneous change notification from the fixed block 8 to
    // block 0x11, with the revision as a 5-byte little-endian 7-bit field.
    auto note = svc.notification(0x02);  // INSTRUMENTS_CHANGED
    CHECK(GmbSysEx::isWellFormed(note.data(), note.size()));
    CHECK_EQ((int)note.size(), 12);
    CHECK_EQ((int)note[3], 0x11);  // v2 change-notification block
    CHECK_EQ((int)note[4], 0x02);  // spontaneous notification
    uint32_t rev = note[5] | (note[6] << 7) | (note[7] << 14) |
                   ((uint32_t)note[8] << 21) | ((uint32_t)(note[9] & 0x0F) << 28);
    CHECK_EQ((int)rev, 2);
}

// ---- multi-transport note identity (audit: DIN + USB + Wi-Fi share the loop) --
//
// Every transport feeds the SAME InstrumentController, and a note used to be
// identified by (channel, note) alone. Two controllers playing the same note on
// the same channel is not a corner case — it is what happens the moment someone
// plugs a DIN cable into a machine that is also on Wi-Fi.

// Stamp source AND origin, exactly as MidiParser::setSource does for a real
// transport: a DIN cable has one sender, so its events all carry MidiOrigin::kDin.
static MidiEvent from(MidiEvent e, MidiSource src) {
    e.source = static_cast<uint8_t>(src);
    e.origin = defaultOriginFor(src);
    return e;
}

// Two DIFFERENT network peers on the same transport. This is what the origin
// exists for: MidiSource::WifiUdp is one transport and the default policy accepts
// any host, so without a per-peer id two laptops are indistinguishable.
static MidiEvent fromPeer(MidiEvent e, uint8_t peer) {
    e.source = static_cast<uint8_t>(MidiSource::WifiUdp);
    e.origin = static_cast<uint8_t>(MidiOrigin::kFirstNetworkPeer + peer);
    return e;
}

// The failure this prevents: DIN's Note Off releasing Wi-Fi's string, leaving
// DIN's own string pressed and ringing with nothing left to release it.
TEST(note_off_only_releases_its_own_sources_note) {
    Profile p = ukulele();
    p.midi.omni = true;
    p.midi.chordWindowMs = 0;   // no grouping window: each note starts at once
    InstrumentController ic;
    ic.load(p);

    // Note 72 is reachable on every string, so the allocator really can give the
    // two senders one each. (60 is the C string's OPEN note: both senders would
    // land on the same physical string, where identity is unobservable.)
    ic.handleEvent(from(noteOn(0, 72, 100), MidiSource::WifiUdp), 0);
    ic.tick(1000);
    ic.handleEvent(from(noteOn(0, 72, 100), MidiSource::Din), 1000);
    ic.tick(2000);
    CHECK_EQ(ic.soundingCount(), 2);   // two senders, two strings

    // DIN releases ITS note. Wi-Fi's must keep sounding.
    ic.handleEvent(from(noteOff(0, 72), MidiSource::Din), 3000);
    ic.tick(4000);
    CHECK_EQ(ic.soundingCount(), 1);

    // ...and Wi-Fi can still release its own, leaving nothing stuck.
    ic.handleEvent(from(noteOff(0, 72), MidiSource::WifiUdp), 5000);
    ic.tick(6000);
    CHECK_EQ(ic.soundingCount(), 0);
}

// A Note Off from a source that never played the note must do nothing at all —
// not "release the closest match".
TEST(note_off_from_a_silent_source_releases_nothing) {
    Profile p = ukulele();
    p.midi.omni = true;
    p.midi.chordWindowMs = 0;
    InstrumentController ic;
    ic.load(p);
    ic.handleEvent(from(noteOn(0, 60, 100), MidiSource::WifiUdp), 0);
    ic.tick(1000);
    CHECK_EQ(ic.soundingCount(), 1);
    ic.handleEvent(from(noteOff(0, 60), MidiSource::Usb), 2000);
    ic.tick(3000);
    CHECK_EQ(ic.soundingCount(), 1);   // untouched
}

// The same isolation inside the chord buffer: a Note Off arriving before the
// grouping window flushes must cancel only its own sender's pending note.
TEST(chord_buffer_cancel_is_per_source) {
    Profile p = ukulele();
    p.midi.omni = true;
    p.midi.chordWindowMs = 5;   // notes wait in the buffer
    InstrumentController ic;
    ic.load(p);
    ic.handleEvent(from(noteOn(0, 60, 100), MidiSource::WifiUdp), 0);
    ic.handleEvent(from(noteOn(0, 60, 100), MidiSource::Din), 0);
    // DIN changes its mind before the window closes.
    ic.handleEvent(from(noteOff(0, 60), MidiSource::Din), 1000);
    ic.tick(20000);             // flush the window
    CHECK_EQ(ic.soundingCount(), 1);   // only Wi-Fi's note survived
}

// Sustain is per sender too: one controller's pedal must neither hold nor release
// another's notes.
TEST(sustain_pedal_is_per_source) {
    Profile p = ukulele();
    p.midi.omni = true;
    p.midi.chordWindowMs = 0;
    p.midi.sustainPedal = true;
    InstrumentController ic;
    ic.load(p);

    // Wi-Fi holds its pedal down, DIN does not.
    // 67 and 69 are two different OPEN strings, so neither sender can displace
    // the other by landing on the same one.
    ic.handleEvent(from(cc(0, 64, 127), MidiSource::WifiUdp), 0);
    ic.handleEvent(from(noteOn(0, 67, 100), MidiSource::WifiUdp), 0);
    ic.tick(1000);
    ic.handleEvent(from(noteOn(0, 69, 100), MidiSource::Din), 1000);
    ic.tick(2000);
    CHECK_EQ(ic.soundingCount(), 2);

    // Both release. Wi-Fi's is held by ITS pedal; DIN's is not held by anything.
    ic.handleEvent(from(noteOff(0, 67), MidiSource::WifiUdp), 3000);
    ic.handleEvent(from(noteOff(0, 69), MidiSource::Din), 3000);
    ic.tick(4000);
    CHECK_EQ(ic.soundingCount(), 1);   // only the pedal-held one remains

    // DIN lifting a pedal it never pressed must not drop Wi-Fi's held note.
    ic.handleEvent(from(cc(0, 64, 0), MidiSource::Din), 5000);
    ic.tick(6000);
    CHECK_EQ(ic.soundingCount(), 1);

    // Wi-Fi lifting its own pedal does.
    ic.handleEvent(from(cc(0, 64, 0), MidiSource::WifiUdp), 7000);
    ic.tick(8000);
    CHECK_EQ(ic.soundingCount(), 0);
}

// A CC selection is one sender's statement about the note IT is about to play.
// Keyed on the channel alone, a Note On from another transport consumed it — so a
// controller's tablature position was applied to somebody else's note.
TEST(cc_selection_is_not_consumed_by_another_source) {
    Profile p = ukulele();
    p.midi.omni = true;
    p.midi.chordWindowMs = 0;
    p.selector.enabled = true;
    p.selector.mode = SelectionMode::Hybrid;
    p.selector.perMidiChannel = true;
    // Anticipated pre-positioning also marks a target active, which the scan below
    // would then read instead of the note DIN actually played. Off, so this test
    // observes exactly one thing: whether DIN consumed Wi-Fi's selection.
    p.selector.prepareOnCompleteSelection = false;
    InstrumentController ic;
    ic.load(p);

    // Wi-Fi selects string 2, fret 5 — and does NOT play yet.
    ic.handleEvent(from(cc(0, p.selector.string.ccNumber, 2), MidiSource::WifiUdp), 0);
    ic.handleEvent(from(cc(0, p.selector.fret.ccNumber, 5), MidiSource::WifiUdp), 0);
    // DIN plays a note. It must NOT land on Wi-Fi's selection: with no selection of
    // its own it falls back to automatic allocation.
    ic.handleEvent(from(noteOn(0, 60, 100), MidiSource::Din), 1000);
    ic.tick(2000);
    CHECK_EQ(ic.soundingCount(), 1);
    int played = -1;
    for (size_t i = 0; i < ic.stringCount(); ++i)
        if (ic.target(i).active) played = static_cast<int>(i);
    CHECK(played >= 0);
    if (played >= 0) CHECK(ic.target(played).fret != 5);
}

// ---- two senders on the SAME transport --------------------------------------
//
// The previous fix keyed a note by its transport, which was enough for DIN vs
// Wi-Fi. It is not enough for Wi-Fi vs Wi-Fi: the default UDP policy accepts any
// host, so two laptops both arrive as MidiSource::WifiUdp and the ambiguity comes
// straight back one level down.

// Counting is NOT enough here, and the first version of this test proved it: with
// both peers playing the same note, releasing them in LIFO order gives the same
// COUNT whether the key is the sender or the transport. It passed under a
// deliberate mutation that keyed on the transport. So assert WHICH string stops —
// the identity is the thing under test, not the arithmetic.
static int soleActiveStringOtherThan(const InstrumentController& ic, int excluded) {
    for (size_t i = 0; i < ic.stringCount(); ++i)
        if (ic.target(i).active && static_cast<int>(i) != excluded)
            return static_cast<int>(i);
    return -1;
}

TEST(two_network_peers_do_not_release_each_others_notes) {
    Profile p = ukulele();
    p.midi.omni = true;
    p.midi.chordWindowMs = 0;
    InstrumentController ic;
    ic.load(p);

    // Note 72 is reachable on every string, so the allocator can give one to each.
    ic.handleEvent(fromPeer(noteOn(0, 72, 100), 0), 0);
    ic.tick(1000);
    int stringA = soleActiveStringOtherThan(ic, -1);
    CHECK(stringA >= 0);
    ic.handleEvent(fromPeer(noteOn(0, 72, 100), 1), 1000);
    ic.tick(2000);
    int stringB = soleActiveStringOtherThan(ic, stringA);
    CHECK(stringB >= 0);
    CHECK(stringA != stringB);
    CHECK_EQ(ic.soundingCount(), 2);

    // Peer A — the FIRST to play — releases. Its own string must be the one that
    // stops. Keyed on the transport, this Note Off matches the most recent entry
    // instead, and peer B's string is damped while A's rings on.
    ic.handleEvent(fromPeer(noteOff(0, 72), 0), 3000);
    ic.tick(4000);
    CHECK_EQ(ic.soundingCount(), 1);
    if (stringA >= 0 && stringB >= 0) {
        CHECK(!ic.target(stringA).active);   // A released its own
        CHECK(ic.target(stringB).active);    // B untouched
    }

    ic.handleEvent(fromPeer(noteOff(0, 72), 1), 5000);
    ic.tick(6000);
    CHECK_EQ(ic.soundingCount(), 0);
}

// All Notes Off is a message about the sender's own channel, not a stop button.
// It used to call panic(), so a DIN controller sending CC123 also damped every
// Wi-Fi note, dropped their pedal and wiped their pending selections.
TEST(all_notes_off_is_scoped_to_its_sender) {
    Profile p = ukulele();
    p.midi.omni = true;
    p.midi.chordWindowMs = 0;
    InstrumentController ic;
    ic.load(p);

    ic.handleEvent(from(noteOn(0, 67, 100), MidiSource::WifiUdp), 0);
    ic.tick(1000);
    ic.handleEvent(from(noteOn(0, 69, 100), MidiSource::Din), 1000);
    ic.tick(2000);
    CHECK_EQ(ic.soundingCount(), 2);

    // DIN says All Notes Off. Only DIN's note stops.
    ic.handleEvent(from(cc(0, 123, 0), MidiSource::Din), 3000);
    ic.tick(4000);
    CHECK_EQ(ic.soundingCount(), 1);

    // ...and the instrument is still ARMED, not panicked: the Wi-Fi player can
    // keep going. A real stop is a safety action with its own routes.
    ic.handleEvent(from(noteOn(0, 64, 100), MidiSource::WifiUdp), 5000);
    ic.tick(6000);
    CHECK_EQ(ic.soundingCount(), 2);
}

// CC120 (All Sound Off) has the same reach as CC123 here.
TEST(all_sound_off_is_scoped_to_its_sender) {
    Profile p = ukulele();
    p.midi.omni = true;
    p.midi.chordWindowMs = 0;
    InstrumentController ic;
    ic.load(p);
    ic.handleEvent(fromPeer(noteOn(0, 67, 100), 0), 0);
    ic.handleEvent(fromPeer(noteOn(0, 69, 100), 1), 0);
    ic.tick(1000);
    CHECK_EQ(ic.soundingCount(), 2);
    ic.handleEvent(fromPeer(cc(0, 120, 0), 1), 2000);
    ic.tick(3000);
    CHECK_EQ(ic.soundingCount(), 1);
}
