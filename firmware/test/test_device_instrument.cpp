// P1.13: the DeviceConfig / InstrumentProfile split must be LOSSLESS against the
// combined Profile, and the boundary must be the RIGHT one — device keeps the
// machine (board, pins, network, E-stop wiring, power hardware), instrument keeps
// the tune (identity, MIDI, selection, power caps, pluck, axes + homing, servos).
//
// Getting the boundary wrong is what would break "carry my instrument to another
// controller": homing offsets, for instance, describe the carriages, not the board.
#include "TestFramework.h"
#include "../src/core/configuration/DeviceInstrument.h"

using namespace gmb;

static Profile sample() {
    Profile p = Profile::makeDefault("Guitar", 6, {40, 45, 50, 55, 59, 64}, 12);
    p.boardIdentifier = "esp32-wroom-32";
    p.reserveUsb = false;
    p.automaticPinAssignment = false;
    p.estopNormallyClosed = true;
    p.network.hostname = "gmb-guitar";
    p.network.ssid = "studio";
    p.hardware.oePullup = true;
    p.hardware.estopCutsDriverEnable = true;
    p.hardware.stepperMoveMa = 1200;
    p.hardware.pcaPullups.push_back(PcaPullupNote{1, 3, 4700});
    p.midi.chordWindowMs = 7;
    p.power.staggerMs = 12;
    p.pluck.muteSource = MuteSource::Plectrum;
    p.instrument.polyphonyMax = 4;
    p.strings[0].fretOffsetMm = 13.5;
    p.homing[0].offsetMm = 4.25;
    p.homing[0].sensorActiveHigh = true;
    return p;
}

TEST(device_instrument_split_is_lossless) {
    Profile p = sample();
    Profile back = mergeProfile(deviceConfigOf(p), instrumentProfileOf(p), p);

    // Device half.
    CHECK(back.boardIdentifier == p.boardIdentifier);
    CHECK(back.reserveUsb == p.reserveUsb);
    CHECK(back.automaticPinAssignment == p.automaticPinAssignment);
    CHECK(back.estopNormallyClosed == p.estopNormallyClosed);
    CHECK(back.pins.size() == p.pins.size());
    CHECK(back.network.hostname == p.network.hostname);
    CHECK(back.network.ssid == p.network.ssid);
    CHECK(back.hardware.oePullup == p.hardware.oePullup);
    CHECK(back.hardware.estopCutsDriverEnable == p.hardware.estopCutsDriverEnable);
    CHECK(back.hardware.stepperMoveMa == p.hardware.stepperMoveMa);
    CHECK(back.hardware.pcaPullups.size() == 1);
    CHECK(back.hardware.pcaPullups[0].ohm == 4700);

    // Instrument half.
    CHECK(back.instrument.name == p.instrument.name);
    CHECK(back.instrument.polyphonyMax == p.instrument.polyphonyMax);
    CHECK(back.midi.chordWindowMs == p.midi.chordWindowMs);
    CHECK(back.power.staggerMs == p.power.staggerMs);
    CHECK(back.pluck.muteSource == p.pluck.muteSource);
    CHECK(back.strings.size() == p.strings.size());
    CHECK(back.servos.size() == p.servos.size());
    CHECK_EQ(back.strings[0].fretOffsetMm, p.strings[0].fretOffsetMm);

    // Profile-level metadata rides on the base, so a round trip preserves it.
    CHECK(back.project == p.project);
    CHECK(back.profileVersion == p.profileVersion);
    CHECK(back.capabilitiesRevision == p.capabilitiesRevision);
}

// Homing describes the CARRIAGES, so it must travel with the instrument — carrying
// an instrument to another controller has to bring its homing offsets and sensor
// polarities along, or the first re-home would seek against the wrong reference.
TEST(homing_travels_with_the_instrument_half) {
    Profile p = sample();
    InstrumentProfile ip = instrumentProfileOf(p);
    CHECK(ip.homing.size() == p.homing.size());
    CHECK_EQ(ip.homing[0].offsetMm, 4.25);
    CHECK(ip.homing[0].sensorActiveHigh);

    // Swapping the instrument onto a DIFFERENT device keeps the device's own
    // machine config and takes the instrument's mechanics with it.
    Profile other = Profile::makeDefault("Ukulele", 4, {67, 60, 64, 69}, 12);
    other.boardIdentifier = "esp32-devkit-v1";
    other.network.hostname = "gmb-other";
    other.estopNormallyClosed = false;
    Profile moved = mergeProfile(deviceConfigOf(other), ip, other);
    CHECK(moved.boardIdentifier == "esp32-devkit-v1");   // device kept
    CHECK(moved.network.hostname == "gmb-other");        // device kept
    CHECK(!moved.estopNormallyClosed);                   // device kept
    CHECK(moved.strings.size() == 6);                    // instrument moved
    CHECK(moved.homing.size() == p.homing.size());
    CHECK_EQ(moved.homing[0].offsetMm, 4.25);
    CHECK(moved.instrument.name == "Guitar");
}

// The two halves are disjoint: changing one must not disturb the other.
TEST(device_and_instrument_halves_are_disjoint) {
    Profile p = sample();
    DeviceConfig d = deviceConfigOf(p);
    InstrumentProfile i = instrumentProfileOf(p);

    d.network.hostname = "renamed";
    d.estopNormallyClosed = false;
    i.instrument.name = "Other";
    i.strings.resize(2);
    i.homing.resize(2);

    Profile merged = mergeProfile(d, i, p);
    CHECK(merged.network.hostname == "renamed");
    CHECK(!merged.estopNormallyClosed);
    CHECK(merged.instrument.name == "Other");
    CHECK(merged.strings.size() == 2);
    CHECK(merged.homing.size() == 2);
    // The original is untouched (the split takes copies, never references).
    CHECK(p.network.hostname == "gmb-guitar");
    CHECK(p.instrument.name == "Guitar");
    CHECK(p.strings.size() == 6);
}
