// Wi-Fi MIDI transport (spec §8.2). Initial implementation uses a
// configurable UDP socket carrying raw MIDI bytes; decoded events and SysEx come
// out through the transport-independent MidiParser so BLE/USB/DIN can be added
// later without touching the instrument logic (§8.3).
//
// Each poll() processes a BOUNDED number of packets so a UDP flood cannot
// monopolise loop() or starve the E-stop / LIMIT checks. Every SysEx request is
// tagged with its sender so the response goes back to the right client.
#pragma once

#include <cstdint>
#include <vector>

#include "../../core/midi/MidiParser.h"

#if defined(ARDUINO)
#include <WiFiUdp.h>
#endif

namespace gmb {

struct SysExPacket {
    std::vector<uint8_t> bytes;
#if defined(ARDUINO)
    IPAddress ip;
#endif
    uint16_t port = 0;
};

class MidiWifi {
public:
    void begin(uint16_t port = 5006);

    // Pull a bounded number of received packets and decode them.
    void poll(uint32_t nowUs);

    // Decoded channel-voice events for this poll (consume then clear()).
    std::vector<MidiEvent>& events() { return events_; }
    // Complete SysEx requests received this poll, each with its sender.
    std::vector<SysExPacket>& sysexPackets() { return sysex_; }
    void clear() {
        events_.clear();
        sysex_.clear();
    }

    // Reply to a specific SysEx sender.
    void reply(const SysExPacket& to, const uint8_t* data, size_t len);

    // Send an UNSOLICITED message (e.g. a capabilities-changed notification) to
    // the last host that made a SysEx request. Returns false if no host is known
    // yet. Used so a runtime capability change is pushed, not only polled.
    bool notifyLastSender(const uint8_t* data, size_t len);
    bool hasLastSender() const { return lastSenderPort_ != 0; }

    // Dropped-input counters (oversized datagrams / per-tick event overflow) so a
    // sustained MIDI overflow is observable rather than silent.
    uint32_t droppedEvents() const { return droppedEvents_; }
    uint32_t droppedPackets() const { return droppedPackets_; }

private:
    static constexpr int kMaxPacketsPerTick = 8;
    static constexpr size_t kMaxEventsPerTick = 128;

    MidiParser parser_;
    uint16_t port_ = 5006;
    std::vector<MidiEvent> events_;
    std::vector<SysExPacket> sysex_;
    uint16_t lastSenderPort_ = 0;
    uint32_t droppedEvents_ = 0;
    uint32_t droppedPackets_ = 0;
#if defined(ARDUINO)
    WiFiUDP udp_;
    IPAddress lastSenderIp_;
    uint8_t buf_[512];
#endif
};

}  // namespace gmb
