// Configurable, per-board GPIO description (spec section 11).
//
// The firmware must NOT use a single global pin list for every board. Each board
// ships a profile describing what every exposed GPIO can do, so the pin manager
// and the web UI can filter choices per signal and per board variant.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gmb {

// UI colour category (spec 11.2).
enum class PinPreference : uint8_t {
    Recommended = 0,  // green
    Caution = 1,      // yellow — advanced mode only, with explanation
    Reserved = 2,     // red — not selectable
    Used = 3,         // grey — already assigned (runtime state, not static)
};

// Static capabilities of a single physical GPIO on a given board.
struct PinCapability {
    int8_t gpio = -1;
    bool exposed = false;          // broken out on the board header
    bool input = false;
    bool output = false;
    bool interrupt = false;
    bool highSpeedOutput = false;  // suitable for STEP / fast toggling
    bool internalPullUp = false;
    bool internalPullDown = false;
    bool adc = false;
    bool reserved = false;         // reserved by firmware policy (e.g. future USB)
    bool strapping = false;        // boot strapping pin
    bool usb = false;              // USB-JTAG / native USB
    bool onboardPeripheral = false; // wired to an on-board device (LED, UART...)
    PinPreference preference = PinPreference::Caution;
    std::string note;              // human-readable reason, shown in the UI
};

// The kind of signal a pin is being requested for. Used to filter candidates
// (spec 11.3).
enum class SignalKind : uint8_t {
    Step,     // stepper STEP — needs fast output
    Dir,      // stepper DIR — plain output
    Enable,   // driver ENABLE — plain output
    Home,     // homing sensor — input + interrupt + pull
    Limit,    // opposite end-stop — input + interrupt + pull
    Diag,     // TMC2209 DIAG — input
    I2cSda,   // PCA9685 SDA
    I2cScl,   // PCA9685 SCL
    ServoOe,  // PCA9685 output-enable / safety
    Generic,  // any usable output
    SafetyInput,  // hardware E-stop input (`ESTOP`): input + interrupt + internal
                  // pull-up, never a strapping pin (an NC loop idles the pin LOW
                  // through boot, which would corrupt the boot strap)
};

struct BoardProfile {
    std::string identifier;
    std::string displayName;
    std::vector<PinCapability> pins;

    const PinCapability* find(int8_t gpio) const;
    // Pins that can legally carry the given signal, in preference order
    // (recommended first). Reserved / non-exposed pins are never returned.
    std::vector<const PinCapability*> candidatesFor(SignalKind kind) const;

    // Whether a given pin may carry a given signal (ignoring current usage).
    bool supports(int8_t gpio, SignalKind kind) const;
};

// Built-in profiles (spec 11.4 / 11.5). The S3-DevKitC-1 is the reference board;
// the classic ESP32-WROOM-32 (38-pin DevKitC) and ESP32 DevKit v1 (30-pin) are
// the common cheaper boards — same die, so they share a GPIO capability model and
// differ only in which pins are broken out.
//
// Espressif shipped TWO revisions of the ESP32-S3-DevKitC-1 that differ in where
// the on-board RGB LED (WS2812) sits: the initial release drives it from GPIO48,
// the v1.1 revision from GPIO38. The pin the LED occupies must stay reserved, so
// each revision is its own profile — check the silkscreen / Espressif user guide
// to pick the right one. `esp32-s3-devkitc-1` keeps naming the original (v1.0)
// board so existing stored profiles keep their meaning.
//
// A classic ESP32 has only 8 usable high-speed outputs left after the STEP lines,
// so a 6-axis instrument is realistically an S3 job; the smaller boards suit 1..3
// strings. The validator enforces the real per-board limits either way.
BoardProfile makeEsp32S3DevKitC1();     // v1.0 — RGB LED on GPIO48, GPIO38 free
BoardProfile makeEsp32S3DevKitC1V11();  // v1.1 — RGB LED on GPIO38, GPIO48 free
BoardProfile makeEsp32Wroom32();
BoardProfile makeEsp32DevKitV1();

// Every built-in profile, in menu order. The single source of truth for "which
// boards does this firmware support" — the web UI enumerates it rather than
// carrying its own list, which is how the picker ended up offering one board
// while the firmware supported four.
const std::vector<const BoardProfile*>& builtinBoardProfiles();

// Returns the built-in profile with a matching identifier, or nullptr.
const BoardProfile* builtinBoardProfile(const std::string& identifier);

}  // namespace gmb
