// Regression tests for the second audit's P0/P1 findings.
#include "TestFramework.h"
#include "../src/core/configuration/Profile.h"
#include "../src/core/configuration/ProfileValidator.h"
#include "../src/core/gmb/Capabilities.h"
#include "../src/core/instrument/InstrumentController.h"
#include "../src/core/midi/MidiParser.h"
#include "../src/core/midi/StringFretSelector.h"
#include "../src/core/motion/StepperAxis.h"

using namespace gmb;

static Profile uke() {
    return Profile::makeDefault("Ukulele", 4, {67, 60, 64, 69}, 12);
}
static MidiEvent cc(uint8_t ch, uint8_t n, uint8_t v, uint32_t t = 0) {
    MidiEvent e; e.type = (uint8_t)MidiType::ControlChange;
    e.channel = ch; e.data1 = n; e.data2 = v; e.timestampUs = t; return e;
}
static MidiEvent noteOn(uint8_t ch, uint8_t note, uint8_t vel, uint32_t t = 0) {
    MidiEvent e; e.type = (uint8_t)MidiType::NoteOn;
    e.channel = ch; e.data1 = note; e.data2 = vel; e.timestampUs = t; return e;
}
static MidiEvent noteOff(uint8_t ch, uint8_t note, uint32_t t = 0) {
    MidiEvent e; e.type = (uint8_t)MidiType::NoteOff;
    e.channel = ch; e.data1 = note; e.timestampUs = t; return e;
}

// --- Validator hardening (P0 #3) ---

TEST(validator_requires_mandatory_pins) {
    Profile p = uke();
    // Drop the STEP1 pin.
    for (auto it = p.pins.begin(); it != p.pins.end(); ++it)
        if (it->signal == "STEP1") { p.pins.erase(it); break; }
    CHECK(!ProfileValidator::isActivatable(p));
}

TEST(validator_requires_i2c_and_oe_for_pca) {
    Profile p = uke();  // default servos are PCA
    for (auto it = p.pins.begin(); it != p.pins.end();) {
        if (it->signal == "SDA" || it->signal == "SCL" || it->signal == "SERVO_OE")
            it = p.pins.erase(it);
        else ++it;
    }
    CHECK(!ProfileValidator::isActivatable(p));
}

TEST(validator_rejects_zero_microsteps) {
    Profile p = uke();
    p.strings[0].microsteps = 0;
    CHECK(!ProfileValidator::isActivatable(p));
}

TEST(validator_rejects_nonmonotonic_frets) {
    Profile p = uke();
    p.strings[0].calibratedFretMm = {0.0, 30.0, 20.0};  // goes backward
    CHECK(!ProfileValidator::isActivatable(p));
}

TEST(validator_rejects_out_of_range_rest_pulse) {
    Profile p = uke();
    p.servos[0].restUs = 3000;  // > pulseMaxUs (2500)
    CHECK(!ProfileValidator::isActivatable(p));
}

TEST(validator_requires_pluck_servo_for_individual_mode) {
    Profile p = uke();
    p.instrument.pluckMode = PluckMode::Individual;
    // Remove all pluck servos.
    for (auto it = p.servos.begin(); it != p.servos.end();) {
        if (it->function == "pluck") it = p.servos.erase(it);
        else ++it;
    }
    CHECK(!ProfileValidator::isActivatable(p));
}

TEST(validator_requires_shared_strum_servo) {
    Profile p = uke();
    p.instrument.pluckMode = PluckMode::SharedStrum;
    CHECK(!ProfileValidator::isActivatable(p));  // no sharedStrum servo present
    ServoConfig strum;
    strum.enabled = true; strum.function = "sharedStrum"; strum.stringIndex = -1;
    strum.channel = 12;
    p.servos.push_back(strum);
    CHECK(ProfileValidator::isActivatable(p));
}

TEST(validator_limits_direct_servos_to_eight) {
    Profile p = uke();
    for (int i = 0; i < 9; ++i) {
        ServoConfig s; s.enabled = true; s.function = "aux"; s.stringIndex = -1;
        s.source = ServoSource::DirectGpio; s.gpio = -1;  // gpio conflict aside
        p.servos.push_back(s);
    }
    bool over = false;
    for (const auto& is : ProfileValidator::validate(p))
        if (is.field == "servos.direct") over = true;
    CHECK(over);
}

// --- MIDI P1 ghost note (chord buffer) ---

TEST(note_off_cancels_buffered_chord_note) {
    InstrumentController ic;
    ic.load(uke());
    ic.handleEvent(noteOn(0, 62, 100, 0), 0);   // buffered (automatic)
    ic.handleEvent(noteOff(0, 62, 1000), 1000); // off before the window flushes
    ic.tick(5000);                              // window elapsed
    CHECK_EQ(ic.soundingCount(), 0);            // no ghost note
}

// --- MIDI P1 occupied-string replacement (explicit selection) ---

TEST(explicit_replacement_on_same_string) {
    Profile p = uke();
    p.selector.mode = SelectionMode::Explicit;
    InstrumentController ic;
    ic.load(p);
    // First note on physical string index 2 (open 64), fret 5 -> note 69.
    ic.handleEvent(cc(0, 20, 3, 0), 0);
    ic.handleEvent(cc(0, 21, 5, 0), 0);
    ic.handleEvent(noteOn(0, 69, 100, 0), 0);
    CHECK(ic.target(2).active);
    uint32_t firstId = ic.target(2).commandId;
    // Second note reuses the SAME string (index 2), fret 7 -> note 71.
    ic.handleEvent(cc(0, 20, 3, 0), 0);
    ic.handleEvent(cc(0, 21, 7, 0), 0);
    ic.handleEvent(noteOn(0, 71, 100, 0), 0);
    CHECK(ic.target(2).commandId != firstId);
    // Note Off for the FIRST note must NOT stop the string (old mapping dropped).
    ic.handleEvent(noteOff(0, 69, 0), 0);
    CHECK(ic.target(2).active);
    // Note Off for the current note releases it.
    ic.handleEvent(noteOff(0, 71, 0), 0);
    CHECK(!ic.target(2).active);
}

// --- P0: direction inversion is applied in ONE layer only ---
// mm<->steps must be sign-preserving regardless of invertDirection (the driver
// DIR pin owns inversion), so position and velocity modes agree.
TEST(mm_to_steps_is_not_inverted) {
    AxisConfig cfg;
    cfg.transmission = Transmission::BeltGt2;
    cfg.stepsPerRevolution = 200; cfg.microsteps = 16;
    cfg.pulleyTeeth = 20; cfg.beltPitchMm = 2.0;  // 80 steps/mm
    cfg.invertDirection = true;
    StepperAxis axis(cfg);
    CHECK_EQ(axis.mmToSteps(10.0), 800);   // positive, not -800
    CHECK_NEAR(axis.stepsToMm(800), 10.0, 1e-9);
}

// --- P1: a disabled axis never plays, even via explicit CC selection ---
TEST(disabled_axis_never_plays) {
    Profile p = uke();
    p.strings[0].enabled = false;
    p.selector.mode = SelectionMode::Explicit;
    InstrumentController ic;
    ic.load(p);
    ic.handleEvent(cc(0, 20, 1, 0), 0);   // string 1 -> physical index 0 (disabled)
    ic.handleEvent(cc(0, 21, 0, 0), 0);   // fret 0 -> note 67
    ic.handleEvent(noteOn(0, 67, 100, 0), 0);
    CHECK(!ic.target(0).active);          // refused
    CHECK_EQ(ic.soundingCount(), 0);
}

// --- P1: the selector is cleared on profile load / panic ---
TEST(selector_reset_clears_pending) {
    StringFretSelector sel;
    SelectorConfig cfg; cfg.mode = SelectionMode::Explicit;
    cfg.string.maximum = 4; cfg.fret.maximum = 12;
    sel.configure(cfg);
    InstrumentView v; v.stringCount = 4; v.openNotes = {67, 60, 64, 69};
    v.maxFretPerString = {12, 12, 12, 12};
    sel.setInstrument(v);
    sel.onControlChange(cc(0, 20, 3, 0));
    sel.onControlChange(cc(0, 21, 5, 0));
    CHECK(sel.pending().size() > 0);
    sel.reset();
    CHECK_EQ((int)sel.pending().size(), 0);
}

// --- P1: SysEx parser ignores real-time bytes and bounds the buffer ---
TEST(sysex_parser_ignores_realtime_and_bounds) {
    MidiParser p;
    // A real-time clock byte (0xF8) injected mid-SysEx must not enter the buffer.
    uint8_t msg[] = {0xF0, 0x7D, 0x00, 0xF8, 0x06, 0x01, 0xF7};
    p.feed(msg, sizeof(msg), 0);
    CHECK_EQ((int)p.sysex().size(), 1);
    CHECK_EQ((int)p.sysex()[0].size(), 6);  // 0xF8 dropped: F0 7D 00 06 01 F7
    for (uint8_t b : p.sysex()[0]) CHECK(b != 0xF8);

    // An unterminated oversized SysEx is aborted, not buffered without bound.
    MidiParser q;
    q.feed(0xF0, 0);
    for (size_t i = 0; i < MidiParser::kMaxSysExBytes + 100; ++i) q.feed(0x01, 0);
    CHECK_EQ((int)q.sysex().size(), 0);  // never completed, buffer released
}

// --- P0: a runtime-faulted axis is removed from the allocator ---
TEST(faulted_string_removed_from_allocation) {
    InstrumentController ic;
    ic.load(uke());              // GCEA: 67,60,64,69
    ic.faultString(0);           // take string 0 (G) out of service
    // A full open chord: 60->s1, 64->s2, 69->s3; 67 can only go on s0 (faulted)
    // -> dropped. So exactly 3 sound and string 0 stays inactive.
    ic.handleEvent(noteOn(0, 67, 100, 0), 0);
    ic.handleEvent(noteOn(0, 60, 100, 0), 0);
    ic.handleEvent(noteOn(0, 64, 100, 0), 0);
    ic.handleEvent(noteOn(0, 69, 100, 0), 0);
    ic.tick(5000);
    CHECK(!ic.target(0).active);
    CHECK_EQ(ic.soundingCount(), 3);
}

// --- P0: degraded capabilities exclude the faulted axis entirely ---
TEST(degraded_snapshot_excludes_disabled_string) {
    Profile p = Profile::makeDefault("Guitar", 6, {40, 45, 50, 55, 59, 64}, 12);
    CapabilitySnapshot full = buildSnapshot(p);
    CHECK_EQ((int)full.stringConfig.stringCount, 6);
    CHECK_EQ((int)full.stringConfig.tuning.size(), 6);

    p.strings[0].enabled = false;  // low E axis failed homing (runtime copy)
    CapabilitySnapshot deg = buildSnapshot(p);
    CHECK_EQ((int)deg.stringConfig.stringCount, 5);
    CHECK_EQ((int)deg.stringConfig.tuning.size(), 5);
    CHECK_EQ((int)deg.stringConfig.fretsPerString.size(), 5);
    CHECK_EQ((int)deg.capabilities.polyphony, 5);
    CHECK_EQ((int)deg.capabilities.noteMin, 45);  // low E gone -> A2 is the floor
}

// --- selection spec: fret-then-string CC order ---

TEST(fret_before_string_cc_order) {
    StringFretSelector sel;
    SelectorConfig cfg;
    cfg.mode = SelectionMode::Explicit;
    cfg.string.maximum = 4;
    cfg.fret.maximum = 12;
    sel.configure(cfg);
    InstrumentView v;
    v.stringCount = 4; v.openNotes = {67, 60, 64, 69};
    v.maxFretPerString = {12, 12, 12, 12};
    sel.setInstrument(v);

    // Fret arrives BEFORE the string this time.
    sel.onControlChange(cc(0, 21, 5, 0));  // fret 5
    sel.onControlChange(cc(0, 20, 3, 0));  // string 3 -> index 2
    NoteResolution r = sel.onNoteOn(noteOn(0, 69, 100, 0), 1);
    CHECK(r.play);
    CHECK(r.source == ResolveSource::Explicit);
    CHECK_EQ((int)r.stringIndex, 2);
    CHECK_EQ((int)r.fret, 5);
    CHECK_EQ((int)sel.pending().size(), 0);  // exactly one selection consumed
}
