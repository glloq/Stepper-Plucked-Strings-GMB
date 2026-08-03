// Stepper-Plucked-Strings-GMB — core shared types and constants.
//
// This header is pure C++17 with no Arduino / ESP-IDF dependency so that the
// whole algorithmic core can be unit-tested natively on a host with g++.
#pragma once

#include <cstdint>

namespace gmb {

// Hard capacity limits derived from the spec (section 6).
constexpr uint8_t kMaxStrings = 6;      // 1..6 strings / stepper axes / fingers
constexpr uint8_t kMaxServoOutputs = 16; // PCA9685 channels
constexpr uint8_t kMaxAuxPower = 8;
constexpr uint8_t kMinProfiles = 8;

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
