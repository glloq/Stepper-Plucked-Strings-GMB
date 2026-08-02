// Regression tests for the second audit's P0/P1 findings.
#include "TestFramework.h"
#include "../src/core/configuration/Profile.h"
#include "../src/core/configuration/ProfileValidator.h"
#include "../src/core/instrument/InstrumentController.h"
#include "../src/core/midi/StringFretSelector.h"

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
