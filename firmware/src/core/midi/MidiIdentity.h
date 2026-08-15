// What identifies a note, once the instrument has more than one input.
//
// With a single transport, (channel, note) is a fine key: a Note Off can only have
// come from the thing that sent the Note On. The moment DIN, USB and Wi-Fi all feed
// the same InstrumentController — which is the design, and is now real — it stops
// being one:
//
//     Wi-Fi   NoteOn  ch1 C4     -> string 3 moves, presses, plucks
//     DIN     NoteOn  ch1 C4     -> string 5 moves, presses, plucks
//     DIN     NoteOff ch1 C4     -> finds (ch1, C4)... and releases string 3
//
// The DIN player's Note Off damped the Wi-Fi player's string, and the DIN string is
// now sounding with nothing left to release it: it stays pressed and ringing until
// something else happens to match it. That is a stuck note on a machine with a
// finger held against a string, not just a wrong lookup.
//
// So the key is (source, channel, note). `sourceKey()` folds MidiSource down to the
// 4 bits it needs, and `noteKey()` packs the triple into one comparable value —
// small enough to store per active note and per pending selection without growing
// those structures meaningfully.
//
// Deliberately NOT a per-source *instrument*: the strings are shared hardware, and
// two controllers playing at once must still share the allocator, the polyphony
// limit and the safety state. Only the identity of a note is split.
#pragma once

#include <cstdint>

#include "MidiEvent.h"

namespace gmb {

// MidiSource currently spans 0..8; 4 bits leaves room to double that before this
// needs revisiting, and keeps the packed key in 16 bits.
inline uint8_t sourceKey(uint8_t source) { return static_cast<uint8_t>(source & 0x0F); }

// (source, channel, note) packed into one value: sscc cccc cnnn nnnn is not quite
// it — channel is masked to 4 bits and note to 7, so the layout is
//   bits 15..11  source (4, top bit unused)
//   bits 10..7   channel (4)
//   bits  6..0   note (7)
inline uint16_t noteKey(uint8_t source, uint8_t channel, uint8_t note) {
    return static_cast<uint16_t>((static_cast<uint16_t>(sourceKey(source)) << 11) |
                                 (static_cast<uint16_t>(channel & 0x0F) << 7) |
                                 static_cast<uint16_t>(note & 0x7F));
}

inline uint16_t noteKey(const MidiEvent& e) {
    return noteKey(e.source, e.channel, e.data1);
}

// The key for anything scoped to a sender rather than to a single note: pending CC
// selections, the sustain pedal, "the last valid selection". Same reasoning — a
// controller's CC20 must not complete another controller's Note On.
inline uint8_t sourceChannelKey(uint8_t source, uint8_t channel) {
    return static_cast<uint8_t>((sourceKey(source) << 4) | (channel & 0x0F));
}

inline uint8_t sourceChannelKey(const MidiEvent& e) {
    return sourceChannelKey(e.source, e.channel);
}

}  // namespace gmb
