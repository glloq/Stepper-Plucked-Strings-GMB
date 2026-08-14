// Stepper-Plucked-Strings-GMB — core shared types and constants.
//
// This header is pure C++17 with no Arduino / ESP-IDF dependency so that the
// whole algorithmic core can be unit-tested natively on a host with g++.
#pragma once

#include <cstdint>

namespace gmb {

// Hard capacity limits derived from the spec (section 6).
// The kMaxStrings / kMaxFret bounds and the frettedNote() gamme law below are the
// three points that assume a 6-string, 24-fret, note=open+fret instrument. They are
// intentionally centralised here; docs/GENERALIZATION.md (audit P2.18) maps every
// dependent site and sketches the future Voice/Course abstraction. Do not scatter
// new copies.
constexpr uint8_t kMaxStrings = 6;      // 1..6 strings / stepper axes / fingers
constexpr uint8_t kMaxServoOutputs = 16; // PCA9685 channels per board
constexpr uint8_t kMaxAuxPower = 8;
constexpr uint8_t kMinProfiles = 8;

// Highest fret index the selector / capability blocks can address.
constexpr uint8_t kMaxFret = 24;

// Up to eight PCA9685 boards per I2C bus (addresses 0x40..0x47), on either of the
// ESP32-S3's two hardware I2C controllers — so up to 16 distinct boards. Spreading
// boards over the two buses halves the traffic and refreshes the servos faster.
constexpr uint8_t kMaxPca = 8;

// A MIDI CC number is 7-bit. 120..127 are Channel Mode messages and must not be
// offered as string/fret selectors.
constexpr uint8_t kMaxAssignableCc = 119;

// Sentinel used across the code base for "no GPIO / not assigned".
constexpr int8_t kNoPin = -1;

// Convert a fret index to the theoretical position along the vibrating string.
// position = scaleLengthMm * (1 - 2^(-fret/12))    (spec 14.2)
double fretPositionMm(double scaleLengthMm, int fret);

// Equal-tempered MIDI note produced by an open string at a given fret.
// note = openNote + fret + capo + transpose
inline int frettedNote(int openNote, int fret, int capo = 0, int transpose = 0) {
    return openNote + fret + capo + transpose;
}

// Clamp helper (std::clamp needs <algorithm>; keep this header light).
template <typename T>
inline T clampValue(T v, T lo, T hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace gmb
