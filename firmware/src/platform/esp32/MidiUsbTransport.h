// USB-MIDI transport skeleton for the ESP32-S3 native USB (audit P1.7).
//
// Structure only: it conforms to MidiTransport so it can be registered next to the
// Wi-Fi UDP transport and feed the same InstrumentController with ZERO change to the
// instrument logic. poll() is a documented no-op until wired to the ESP32-S3 native
// USB-MIDI (TinyUSB) stack — the class exists so that wiring is a self-contained
// follow-up, not a cross-cutting change.
//
// Only the ESP32-S3 (and other native-USB parts) can provide this; the classic ESP32
// has no native USB (see platformio.ini — the classic envs omit ARDUINO_USB_*). The
// header compiles on every target so the transport list is uniform.
#pragma once

#include <vector>

#include "../../core/midi/MidiParser.h"
#include "../../core/midi/MidiTransport.h"

namespace gmb {

class MidiUsbTransport : public MidiTransport {
public:
    // Bring the native USB-MIDI device up. No-op skeleton for now.
    void begin() {
        parser_.setSource(MidiSource::Usb);
        // TODO(P1.7): initialise the ESP32-S3 native USB-MIDI (TinyUSB) endpoint.
    }

    void poll(uint32_t nowUs) override {
        (void)nowUs;
        // TODO(P1.7): read USB-MIDI packets, feed parser_.feed(...), collect events_.
        // Left empty so an unconnected/unwired USB device contributes nothing.
    }

    std::vector<MidiEvent>& events() override { return events_; }
    void clear() override { events_.clear(); }
    MidiSource source() const override { return MidiSource::Usb; }
    const char* name() const override { return "usb"; }

private:
    MidiParser parser_;
    std::vector<MidiEvent> events_;
};

}  // namespace gmb
