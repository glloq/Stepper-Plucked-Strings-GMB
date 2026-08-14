// Native USB-MIDI transport for the ESP32-S3 (audit P1.7).
//
// Adafruit_USBD_MIDI is a Stream: it de-packetises the 4-byte USB-MIDI packets and
// hands back a plain MIDI byte stream. So this transport is the same three lines as
// the DIN one — read bytes, feed MidiParser — and the instrument logic never learns
// where a note came from.
//
// OPT-IN, and deliberately so. Enabling it costs more than adding a library:
//
//   * The ESP32-S3 has ONE USB peripheral and two mutually exclusive Arduino modes.
//     The default build uses ARDUINO_USB_MODE=1 (hardware USB-CDC/JTAG), which is
//     what gives you a serial monitor and a plain `esptool` reflash. TinyUSB needs
//     ARDUINO_USB_MODE=0. You cannot have both.
//   * With TinyUSB the console moves onto the emulated CDC, which is absent while
//     the device is enumerating or wedged — so a firmware that hangs early gives you
//     no log, and recovery is a manual BOOT-button reflash.
//   * The classic ESP32 has no native USB at all. There is nothing to enable there.
//
// It is therefore behind GMB_USB_MIDI, set only by the `esp32-s3-usbmidi` PlatformIO
// env. The default S3 image is unchanged. Without the flag this compiles to the same
// inert transport it has always been, so the transport list stays uniform on every
// board and `/api/status` reports it as not implemented in that build.
//
// STATUS: compiled in CI, NOT validated on hardware — nobody has enumerated this
// against a host yet, and there are known S3-specific enumeration quirks in the
// Adafruit TinyUSB tracker. Treat the first flash as a bench experiment: confirm the
// device enumerates as a MIDI port and that the serial console still reaches you
// BEFORE relying on it. Wi-Fi UDP and DIN are the validated inputs.
#pragma once

#include <vector>

#include "../../core/midi/MidiParser.h"
#include "../../core/midi/MidiTransport.h"

#if defined(GMB_USB_MIDI)
#include <Adafruit_TinyUSB.h>
#endif

namespace gmb {

class MidiUsbTransport : public MidiTransport {
public:
    // Bring the native USB-MIDI device up. Returns whether this build has one.
    bool begin() {
        parser_.setSource(MidiSource::Usb);
#if defined(GMB_USB_MIDI)
        // Name the port before begin(): the descriptor is read at enumeration, so a
        // later rename is invisible to the host.
        usb_.setStringDescriptor("GMB Plucked Strings");
        usb_.begin();
        ready_ = true;
#endif
        return ready_;
    }

    // True when this build actually has a USB-MIDI endpoint AND the host has
    // configured it — an unplugged cable is not a bound transport.
    bool bound() const {
#if defined(GMB_USB_MIDI)
        return ready_ && TinyUSBDevice.mounted();
#else
        return false;
#endif
    }

    void poll(uint32_t nowUs) override {
#if defined(GMB_USB_MIDI)
        if (!ready_) return;
        // Bounded per tick, exactly like the DIN transport: a MIDI flood must not be
        // able to stall the control loop and with it the E-stop check.
        for (int n = 0; n < kMaxBytesPerTick && usb_.available() > 0; ++n) {
            int b = usb_.read();
            if (b < 0) break;
            parser_.feed(static_cast<uint8_t>(b), nowUs);
        }
#else
        (void)nowUs;  // no native USB in this build: contribute nothing
#endif
    }

    // The parser owns this poll's decoded events; clear() empties it (and any partial
    // SysEx) for the next tick.
    std::vector<MidiEvent>& events() override { return parser_.events(); }
    void clear() override { parser_.clear(); }
    MidiSource source() const override { return MidiSource::Usb; }
    const char* name() const override { return "usb"; }

private:
    static constexpr int kMaxBytesPerTick = 64;
    MidiParser parser_;
    bool ready_ = false;
#if defined(GMB_USB_MIDI)
    Adafruit_USBD_MIDI usb_;
#endif
};

}  // namespace gmb
