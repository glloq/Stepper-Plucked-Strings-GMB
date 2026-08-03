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

// Built-in profile for the reference board (spec 11.4 / 11.5).
BoardProfile makeEsp32S3DevKitC1();

// Returns the built-in profile with a matching identifier, or nullptr.
const BoardProfile* builtinBoardProfile(const std::string& identifier);

}  // namespace gmb
