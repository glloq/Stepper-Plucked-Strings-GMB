// Wi-Fi MIDI transport (cahier des charges §8.2). Initial implementation uses a
// configurable UDP socket carrying raw MIDI bytes; the decoded events and SysEx
// buffers come out through the transport-independent MidiParser so BLE/USB/DIN
// can be added later without touching the instrument logic (§8.3).
#pragma once

#include <cstdint>

#include "../../core/midi/MidiParser.h"

#if defined(ARDUINO)
#include <WiFiUdp.h>
#endif

namespace gmb {

class MidiWifi {
public:
    void begin(uint16_t port = 5006);

    // Pull any received bytes and decode them. Call from loop().
    void poll(uint32_t nowUs);

    // Decoded output (consume, then call parser().clear()).
    MidiParser& parser() { return parser_; }

    // Send a MIDI byte buffer (e.g. a SysEx response) back to the last sender.
    void send(const uint8_t* data, size_t len);

private:
    MidiParser parser_;
    uint16_t port_ = 5006;
#if defined(ARDUINO)
    WiFiUDP udp_;
    IPAddress lastRemoteIp_;
    uint16_t lastRemotePort_ = 0;
    uint8_t buf_[512];
#endif
};

}  // namespace gmb
