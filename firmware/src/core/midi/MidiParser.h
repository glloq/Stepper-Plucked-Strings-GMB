// Streaming MIDI byte parser -> MidiEvent + complete SysEx buffers.
//
// Transport-independent (spec §8.2): any byte source (UDP,
// WebSocket, BLE, DIN) feeds bytes in and reads decoded events/SysEx out.
// Handles running status.
#pragma once

#include <cstdint>
#include <vector>

#include "MidiEvent.h"

namespace gmb {

class MidiParser {
public:
    void setSource(MidiSource s) { source_ = static_cast<uint8_t>(s); }

    // Feed one raw MIDI byte.
    void feed(uint8_t b, uint32_t nowUs);
    void feed(const uint8_t* data, size_t len, uint32_t nowUs);

    // Decoded channel-voice events (consume then clear()).
    std::vector<MidiEvent>& events() { return events_; }
    // Complete SysEx messages (F0..F7 inclusive).
    std::vector<std::vector<uint8_t>>& sysex() { return sysex_; }
    void clear() {
        events_.clear();
        sysex_.clear();
    }

    // Reset the streaming state (running status, partial channel message, and any
    // in-progress SysEx) WITHOUT discarding already-decoded output. Call at a hard
    // message boundary — e.g. between two independent UDP datagrams — so running
    // status or a half-finished SysEx from one sender never bleeds into the next.
    void resetStream() {
        status_ = 0;
        dataCount_ = 0;
        expected_ = 0;
        inSysex_ = false;
        sysbuf_.clear();
    }

    // Maximum SysEx message length; longer messages are aborted (RAM guard).
    static constexpr size_t kMaxSysExBytes = 512;

private:
    uint8_t source_ = static_cast<uint8_t>(MidiSource::WifiUdp);
    uint8_t status_ = 0;       // running status
    uint8_t data_[2] = {0, 0};
    int dataCount_ = 0;
    int expected_ = 0;
    bool inSysex_ = false;
    std::vector<uint8_t> sysbuf_;
    std::vector<MidiEvent> events_;
    std::vector<std::vector<uint8_t>> sysex_;

    static int expectedData(uint8_t status);
    void emit(uint32_t nowUs);
};

}  // namespace gmb
