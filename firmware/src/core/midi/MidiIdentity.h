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

// ---- origins -----------------------------------------------------------------
//
// `MidiSource` names a TRANSPORT, and that was the right key while each transport
// had one sender. It is not: the default UDP policy accepts any host, so two
// laptops on the same Wi-Fi both arrive as MidiSource::WifiUdp, and
//
//     PC A -> NoteOn  ch1 C4
//     PC B -> NoteOn  ch1 C4
//     PC B -> NoteOff ch1 C4
//
// is ambiguous again — the same defect as before, one level down. So the key is an
// ORIGIN: a small id for "the thing that sent this", which for a network transport
// is an IP:port and for a cable is the cable.
//
// Ids are reserved per transport so they cannot collide, and the whole space fits
// in the 4 bits the packed key already spent on the source.
namespace MidiOrigin {
constexpr uint8_t kInternal = 0;          // firmware-generated
constexpr uint8_t kDin = 1;               // one cable, one sender
constexpr uint8_t kUsb = 2;               // one host
constexpr uint8_t kWebUiTest = 3;         // the web test tools
constexpr uint8_t kFirstNetworkPeer = 4;  // 4..15: UDP peers, by IP:port
constexpr uint8_t kNetworkPeerCount = 12;
constexpr uint8_t kMaxOrigins = 16;
}  // namespace MidiOrigin

// The origin a transport uses when it can only ever have one sender.
inline uint8_t defaultOriginFor(MidiSource s) {
    switch (s) {
        case MidiSource::Din:       return MidiOrigin::kDin;
        case MidiSource::Usb:       return MidiOrigin::kUsb;
        case MidiSource::WebUiTest: return MidiOrigin::kWebUiTest;
        default:                    return MidiOrigin::kInternal;
    }
}

// 4 bits, which is what the packed key has room for.
inline uint8_t originKey(uint8_t origin) { return static_cast<uint8_t>(origin & 0x0F); }

// (origin, channel, note) packed into one value:
//   bits 15..11  origin (4, top bit unused)
//   bits 10..7   channel (4)
//   bits  6..0   note (7)
inline uint16_t noteKey(uint8_t origin, uint8_t channel, uint8_t note) {
    return static_cast<uint16_t>((static_cast<uint16_t>(originKey(origin)) << 11) |
                                 (static_cast<uint16_t>(channel & 0x0F) << 7) |
                                 static_cast<uint16_t>(note & 0x7F));
}

inline uint16_t noteKey(const MidiEvent& e) {
    return noteKey(e.origin, e.channel, e.data1);
}

// The key for anything scoped to a SENDER rather than to a single note: pending CC
// selections, the sustain pedal, "the last valid selection", and the reach of an
// All-Notes-Off. Same reasoning — one controller's CC20 must not complete another
// controller's Note On, and one controller's CC123 must not damp another's strings.
inline uint8_t senderKeyOf(uint8_t origin, uint8_t channel) {
    return static_cast<uint8_t>((originKey(origin) << 4) | (channel & 0x0F));
}

inline uint8_t senderKeyOf(const MidiEvent& e) {
    return senderKeyOf(e.origin, e.channel);
}

// The origin a packed note key belongs to.
inline uint8_t originOfKey(uint16_t key) {
    return static_cast<uint8_t>((key >> 11) & 0x0F);
}

}  // namespace gmb
