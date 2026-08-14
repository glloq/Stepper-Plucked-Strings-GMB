// DIN-5 / TRS MIDI over a hardware UART (audit P1.7, spec §8.3 priority 3).
//
// Unlike native USB, DIN MIDI needs NO special stack — it is just 31250-baud serial.
// This transport is therefore FUNCTIONAL, not a skeleton: it reads raw bytes from a
// Stream (a begun HardwareSerial) and feeds the transport-independent MidiParser, so
// it drives the same InstrumentController as the Wi-Fi UDP transport. It only needs a
// UART bound to it — pass one to begin() once a device pin config exposes the DIN RX
// pin (the remaining wiring step; the byte→event logic below is complete).
#pragma once

#include <cstdint>
#include <vector>

#include "../../core/midi/MidiParser.h"
#include "../../core/midi/MidiTransport.h"

#if defined(ARDUINO)
#include <Arduino.h>  // Stream (HardwareSerial IS-A Stream)
#endif

namespace gmb {

class MidiDinTransport : public MidiTransport {
public:
#if defined(ARDUINO)
    // `uart` must already be begun at 31250 baud on the DIN RX pin. Null = inert.
    void begin(Stream* uart) {
        uart_ = uart;
        parser_.setSource(MidiSource::Din);
    }
#endif

    void poll(uint32_t nowUs) override {
#if defined(ARDUINO)
        if (!uart_) return;
        // Bounded per tick so a MIDI flood can't stall the control loop / E-stop check.
        for (int n = 0; n < kMaxBytesPerTick && uart_->available() > 0; ++n) {
            int b = uart_->read();
            if (b < 0) break;
            parser_.feed(static_cast<uint8_t>(b), nowUs);
        }
#else
        (void)nowUs;
#endif
    }

    // The parser owns this poll's decoded events; clear() empties it (and any partial
    // SysEx) for the next tick.
    std::vector<MidiEvent>& events() override { return parser_.events(); }
    void clear() override { parser_.clear(); }
    MidiSource source() const override { return MidiSource::Din; }
    const char* name() const override { return "din"; }

private:
    static constexpr int kMaxBytesPerTick = 64;
    MidiParser parser_;
#if defined(ARDUINO)
    Stream* uart_ = nullptr;
#endif
};

}  // namespace gmb
