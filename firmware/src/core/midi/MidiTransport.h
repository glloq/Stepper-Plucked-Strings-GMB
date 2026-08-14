// Abstract MIDI transport (audit P1.7).
//
// The MIDI core (MidiParser / InstrumentController) is already transport-independent:
// every transport decodes raw bytes into MidiEvent. This interface makes that
// explicit so SEVERAL transports can feed the SAME InstrumentController, and so a new
// transport is added without touching the instrument logic.
//
// It captures only the per-tick ingestion contract — poll a bounded batch, hand back
// the decoded events, then clear — plus a source tag for diagnostics/routing.
// Bring-up (begin) and any reply/back-channel are transport-specific and stay on the
// concrete class: UDP needs a port and IP-addressed SysEx replies; USB/DIN/BLE do not.
//
// Priority (spec §8.3): Wi-Fi UDP (existing) → USB-MIDI (ESP32-S3 native) → DIN UART
// → BLE. This header is pure C++17 so a fake transport can be unit-tested on the host.
#pragma once

#include <vector>

#include "MidiEvent.h"

namespace gmb {

class MidiTransport {
public:
    virtual ~MidiTransport() = default;

    // Ingest at most a bounded batch of inbound MIDI for this tick (never blocks the
    // control loop / E-stop check). Decoded channel-voice events land in events().
    virtual void poll(uint32_t nowUs) = 0;

    // Decoded events produced by the last poll(); consume then clear(). Non-const so
    // the caller can move/broadcast them, matching the existing MidiWifi contract.
    virtual std::vector<MidiEvent>& events() = 0;

    // Drop this tick's batch once consumed.
    virtual void clear() = 0;

    // Which transport this is (already stamped onto each MidiEvent.source too), so
    // diagnostics and routing can tell inputs apart.
    virtual MidiSource source() const = 0;

    // Short stable name for logs / diagnostics ("wifiUdp", "usb", "din", "ble").
    virtual const char* name() const = 0;
};

}  // namespace gmb
