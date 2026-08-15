// Transport-independent internal MIDI event (spec 8.2).
//
// Every transport (WebSocket, RTP-MIDI, UDP, BLE/USB later) must decode raw
// MIDI into this single struct so that the rest of the firmware never depends
// on how the bytes arrived.
#pragma once

#include <cstdint>

namespace gmb {

// MIDI status high-nibble message types (channel voice messages).
enum class MidiType : uint8_t {
    NoteOff = 0x80,
    NoteOn = 0x90,
    PolyAftertouch = 0xA0,
    ControlChange = 0xB0,
    ProgramChange = 0xC0,
    ChannelAftertouch = 0xD0,
    PitchBend = 0xE0,
    SystemExclusive = 0xF0,
};

// Identifies which transport produced the event (for diagnostics / routing).
enum class MidiSource : uint8_t {
    Internal = 0,
    WifiWebSocket = 1,
    WifiRtp = 2,
    WifiUdp = 3,
    WebUiTest = 4,
    Ble = 5,
    Usb = 6,
    Din = 7,
    // NOT `Serial`. Arduino's HardwareSerial.h does `#define Serial Serial0`
    // (or HWCDCSerial / USBSerial depending on the USB mode), so ANY translation
    // unit that includes Arduino.h and writes `MidiSource::Serial` is macro-expanded
    // into `MidiSource::Serial0` and fails to compile. The enumerator went unnamed
    // for a long time, which is the only reason it took until a `switch` over every
    // source to find out — on all four hardware targets at once.
    SerialPort = 8,
};

struct MidiEvent {
    uint32_t timestampUs = 0;
    uint8_t source = static_cast<uint8_t>(MidiSource::Internal);
    // WHICH sender, not just which transport. A transport with one cable has one
    // origin; a network transport allocates one per IP:port, because "accept any
    // sender" is the default policy and two hosts on the same Wi-Fi are otherwise
    // indistinguishable. See core/midi/MidiIdentity.h.
    uint8_t origin = 0;
    uint8_t type = 0;      // MidiType value (status high nibble)
    uint8_t channel = 0;   // 0..15 (internal, zero-based)
    uint8_t data1 = 0;
    uint8_t data2 = 0;

    bool isNoteOn() const {
        return type == static_cast<uint8_t>(MidiType::NoteOn) && data2 > 0;
    }
    // A NoteOn with velocity 0 is a running-status NoteOff.
    bool isNoteOff() const {
        return type == static_cast<uint8_t>(MidiType::NoteOff) ||
               (type == static_cast<uint8_t>(MidiType::NoteOn) && data2 == 0);
    }
    bool isControlChange() const {
        return type == static_cast<uint8_t>(MidiType::ControlChange);
    }
};

}  // namespace gmb
