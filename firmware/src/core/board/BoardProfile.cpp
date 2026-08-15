#include "BoardProfile.h"

namespace gmb {

const PinCapability* BoardProfile::find(int8_t gpio) const {
    for (const auto& p : pins) {
        if (p.gpio == gpio) return &p;
    }
    return nullptr;
}

static bool pinSupports(const PinCapability& p, SignalKind kind) {
    if (!p.exposed || p.reserved) return false;
    switch (kind) {
        case SignalKind::Step:
            return p.output && p.highSpeedOutput;
        case SignalKind::Dir:
        case SignalKind::Enable:
        case SignalKind::ServoOe:
        case SignalKind::Generic:
            return p.output;
        case SignalKind::Home:
        case SignalKind::Limit:
            // StepperBank configures both as INPUT_PULLUP, so a pin without an
            // internal pull-up would FLOAT and the endstop would read as noise.
            // (The classic ESP32's input-only 34/35/36/39 are exactly that case.)
            return p.input && p.interrupt && p.internalPullUp;
        case SignalKind::Diag:
            return p.input;
        case SignalKind::SafetyInput:
            // Hardware E-stop (`ESTOP`): a readable, interrupt-capable pin WITH a
            // usable internal pull-up — the firmware samples it as INPUT_PULLUP, so
            // a pin without one (classic-ESP32 input-only 34/35/36/39) would float
            // and the E-stop input could read anything. Never a strapping pin: the
            // recommended NC loop holds the pin LOW whenever the machine is allowed
            // to run, including through a reset, which would corrupt the boot strap.
            return p.input && p.interrupt && p.internalPullUp && !p.strapping;
        case SignalKind::UartRx:
            // DIN MIDI in. The ESP32 UART matrix can route RX to any input pin, so
            // the requirement is just "readable and not a strapping pin": a MIDI
            // line driven by a powered sender can hold the level either way across
            // a reset, which is exactly what a strapping pin must not see.
            return p.input && !p.strapping;
        case SignalKind::I2cSda:
        case SignalKind::I2cScl:
            // I2C is open-drain: needs a pin usable both ways.
            return p.output && p.input;
    }
    return false;
}

bool BoardProfile::supports(int8_t gpio, SignalKind kind) const {
    const PinCapability* p = find(gpio);
    return p != nullptr && pinSupports(*p, kind);
}

std::vector<const PinCapability*> BoardProfile::candidatesFor(SignalKind kind) const {
    std::vector<const PinCapability*> out;
    // Recommended pins first, then caution. Reserved never appear.
    for (int pass = 0; pass < 2; ++pass) {
        PinPreference want = pass == 0 ? PinPreference::Recommended : PinPreference::Caution;
        for (const auto& p : pins) {
            if (p.preference == want && pinSupports(p, kind)) out.push_back(&p);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// ESP32-S3-DevKitC-1 reference profile.
// ---------------------------------------------------------------------------
namespace {

PinCapability normalPin(int8_t gpio, PinPreference pref, bool adc, const char* note = "") {
    PinCapability c;
    c.gpio = gpio;
    c.exposed = true;
    c.input = true;              // every ESP32-S3 GPIO is input+output capable
    c.output = true;
    c.interrupt = true;          // all GPIOs can raise interrupts
    c.highSpeedOutput = true;
    c.internalPullUp = true;
    c.internalPullDown = true;
    c.adc = adc;
    c.preference = pref;
    c.note = note;
    return c;
}

PinCapability reservedPin(int8_t gpio, const char* note, bool strapping = false,
                          bool usb = false, bool onboard = false) {
    PinCapability c;
    c.gpio = gpio;
    c.exposed = true;
    c.input = true;
    c.output = true;
    c.interrupt = true;
    c.highSpeedOutput = false;
    c.reserved = true;
    c.strapping = strapping;
    c.usb = usb;
    c.onboardPeripheral = onboard;
    c.preference = PinPreference::Reserved;
    c.note = note;
    return c;
}

// Classic-ESP32 input-only pins (34/35/36/39).
//
// These are CAUTION, not Reserved. "Cannot drive an output" is not "cannot be used":
// they are perfectly good MIDI-RX and other read-only inputs, and on a chip with very
// few free pins left they are worth having. Marking them Reserved meant
// `candidatesFor()` never returned them and the web UI filtered them out, so the one
// signal they suit — `MIDI_RX` — could not actually be assigned to them even though
// `supports()` accepted them.
//
// What keeps them out of the wrong slots is their CAPABILITIES, which is where the
// truth belongs: output=false bars STEP/DIR/ENABLE//OE/I2C, and internalPullUp=false
// bars HOME/LIMIT/ESTOP (all sampled INPUT_PULLUP — an input-only pin would float).
PinCapability inputOnlyPin(int8_t gpio, const char* note) {
    PinCapability c;
    c.gpio = gpio;
    c.exposed = true;
    c.input = true;
    c.output = false;
    c.interrupt = true;
    c.highSpeedOutput = false;
    c.internalPullUp = false;
    c.internalPullDown = false;
    c.adc = true;
    c.preference = PinPreference::Caution;
    c.note = note;
    return c;
}

}  // namespace

// Shared S3-DevKitC-1 builder. The two board revisions differ ONLY in which GPIO
// carries the on-board RGB LED (WS2812): GPIO48 on the original (v1.0) release,
// GPIO38 on v1.1 — the LED pin is reserved, the other one is free.
static BoardProfile makeEsp32S3DevKitC1Rev(bool ledOnGpio38) {
    BoardProfile b;

    auto add = [&](PinCapability c) { b.pins.push_back(c); };

    // ADC1 covers GPIO1..10, ADC2 covers GPIO11..20 on the ESP32-S3.
    // Strapping / boot pins (spec 11.4).
    // GPIO0 is the BOOT button, which the firmware also samples to force the Wi-Fi
    // hotspot (a long press) — main.cpp drives it as an INPUT the whole time. It must
    // therefore NEVER be auto- or hand-assigned to any signal, or the hotspot escape
    // hatch (and the bootloader entry) would fight that pin (audit P1.15).
    add(reservedPin(0, "Strapping / BOOT button — forces the Wi-Fi hotspot (P1.15)",
                    /*strapping=*/true));
    add(normalPin(1, PinPreference::Recommended, true));
    add(normalPin(2, PinPreference::Recommended, true));
    add(reservedPin(3, "Strapping pin", /*strapping=*/true));
    add(normalPin(4, PinPreference::Recommended, true));
    add(normalPin(5, PinPreference::Recommended, true));
    add(normalPin(6, PinPreference::Recommended, true));
    add(normalPin(7, PinPreference::Recommended, true));
    add(normalPin(8, PinPreference::Recommended, true));
    add(normalPin(9, PinPreference::Recommended, true));
    add(normalPin(10, PinPreference::Recommended, true));
    add(normalPin(11, PinPreference::Recommended, true));
    add(normalPin(12, PinPreference::Recommended, true));
    add(normalPin(13, PinPreference::Recommended, true));
    add(normalPin(14, PinPreference::Recommended, true));
    add(normalPin(15, PinPreference::Recommended, true));
    add(normalPin(16, PinPreference::Recommended, true));
    add(normalPin(17, PinPreference::Recommended, true));
    add(normalPin(18, PinPreference::Recommended, true));
    add(reservedPin(19, "USB-JTAG / native USB D-", /*strapping=*/false, /*usb=*/true));
    add(reservedPin(20, "USB-JTAG / native USB D+", /*strapping=*/false, /*usb=*/true));
    add(normalPin(21, PinPreference::Recommended, false));
    // GPIO22..25 do not exist on the ESP32-S3.
    // SPI0/1 flash & PSRAM lines — not usable.
    for (int g = 26; g <= 32; ++g) {
        add(reservedPin(static_cast<int8_t>(g), "Reserved for on-chip Flash / PSRAM"));
    }
    // Variant-dependent memory pins.
    add(normalPin(33, PinPreference::Caution, false,
                  "May be tied to Flash/PSRAM on some module variants"));
    add(normalPin(34, PinPreference::Caution, false,
                  "May be tied to Flash/PSRAM on some module variants"));
    add(reservedPin(35, "Octal Flash/PSRAM on some variants — verify module"));
    add(reservedPin(36, "Octal Flash/PSRAM on some variants — verify module"));
    add(reservedPin(37, "Octal Flash/PSRAM on some variants — verify module"));
    if (ledOnGpio38)
        add(reservedPin(38, "On-board RGB LED (DevKitC-1 v1.1)", false, false, true));
    else
        add(normalPin(38, PinPreference::Recommended, false));
    add(normalPin(39, PinPreference::Recommended, false));
    add(normalPin(40, PinPreference::Recommended, false, "Recommended I2C SDA"));
    add(normalPin(41, PinPreference::Recommended, false, "Recommended I2C SCL"));
    add(normalPin(42, PinPreference::Recommended, false));
    add(reservedPin(43, "UART0 TX (programming / diagnostics)", false, false, true));
    add(reservedPin(44, "UART0 RX (programming / diagnostics)", false, false, true));
    add(reservedPin(45, "Strapping pin", /*strapping=*/true));
    add(reservedPin(46, "Strapping pin", /*strapping=*/true));
    add(normalPin(47, PinPreference::Recommended, false));
    if (ledOnGpio38)
        add(normalPin(48, PinPreference::Recommended, false));
    else
        add(reservedPin(48, "On-board RGB LED (DevKitC-1 v1.0)", false, false, true));

    return b;
}

BoardProfile makeEsp32S3DevKitC1() {
    BoardProfile b = makeEsp32S3DevKitC1Rev(/*ledOnGpio38=*/false);
    // Historical identifier: names the ORIGINAL (v1.0) revision so profiles stored
    // before the split keep exactly the pin map they were validated against.
    b.identifier = "esp32-s3-devkitc-1";
    b.displayName = "ESP32-S3-DevKitC-1 v1.0 (RGB LED on GPIO48)";
    return b;
}

BoardProfile makeEsp32S3DevKitC1V11() {
    BoardProfile b = makeEsp32S3DevKitC1Rev(/*ledOnGpio38=*/true);
    b.identifier = "esp32-s3-devkitc-1-v1.1";
    b.displayName = "ESP32-S3-DevKitC-1 v1.1 (RGB LED on GPIO38)";
    return b;
}

// ---------------------------------------------------------------------------
// Classic ESP32 (ESP32-WROOM-32 / ESP32-D0WD). Shared GPIO capability model for
// the 38-pin DevKitC and 30-pin DevKit v1 boards; they differ only in whether the
// SPI-flash pads (6..11) are broken out.
//   • Input-only: 34, 35, 36 (VP), 39 (VN) — no output, can't carry our signals.
//   • Strapping (caution): 0, 2, 5, 12, 15.
//   • Reserved: 1/3 (UART0), 6..11 (SPI flash).
//   • GPIO 20, 24, 28..31, 37, 38 do not exist / are not broken out.
// ---------------------------------------------------------------------------
namespace {

void addClassicEsp32Pins(BoardProfile& b, bool includeFlash) {
    auto add = [&](PinCapability c) { b.pins.push_back(c); };
    // Caution-grade strapping pin: usable in advanced mode, but the strapping flag
    // must be set so signals that idle a level through boot (the ESTOP safety
    // input's NC loop) are refused on it.
    auto addStrap = [&](PinCapability c) { c.strapping = true; b.pins.push_back(c); };
    // GPIO0: BOOT button + hotspot escape hatch, reserved on every board (P1.15).
    add(reservedPin(0, "BOOT button — forces the Wi-Fi hotspot; never a signal pin",
                    /*strapping=*/true));
    add(reservedPin(1, "UART0 TX (programming / diagnostics)", false, false, true));
    addStrap(normalPin(2, PinPreference::Caution, true, "Strapping / on-board LED on many boards"));
    add(reservedPin(3, "UART0 RX (programming / diagnostics)", false, false, true));
    add(normalPin(4, PinPreference::Recommended, true));
    addStrap(normalPin(5, PinPreference::Caution, false, "Strapping pin (must be HIGH at boot)"));
    if (includeFlash) {
        for (int g = 6; g <= 11; ++g)
            add(reservedPin(static_cast<int8_t>(g), "Connected to the SPI flash"));
    }
    addStrap(normalPin(12, PinPreference::Caution, true, "MTDI strapping pin (flash voltage)"));
    add(normalPin(13, PinPreference::Recommended, true));
    add(normalPin(14, PinPreference::Recommended, true));
    addStrap(normalPin(15, PinPreference::Caution, true, "MTDO strapping pin"));
    add(normalPin(16, PinPreference::Recommended, false, "Used by PSRAM on WROVER modules — verify yours"));
    add(normalPin(17, PinPreference::Recommended, false, "Used by PSRAM on WROVER modules — verify yours"));
    add(normalPin(18, PinPreference::Recommended, false));
    add(normalPin(19, PinPreference::Recommended, false));
    add(normalPin(21, PinPreference::Recommended, false, "Recommended I2C SDA"));
    add(normalPin(22, PinPreference::Recommended, false, "Recommended I2C SCL"));
    add(normalPin(23, PinPreference::Recommended, false, "Recommended PCA9685 /OE"));
    add(normalPin(25, PinPreference::Recommended, true));
    add(normalPin(26, PinPreference::Recommended, true));
    add(normalPin(27, PinPreference::Recommended, true));
    add(normalPin(32, PinPreference::Recommended, true));
    add(normalPin(33, PinPreference::Recommended, true));
    add(inputOnlyPin(34, "Input-only (ADC1), no internal pull-up — read-only signals such as MIDI_RX; cannot drive an output, and needs an external pull-up"));
    add(inputOnlyPin(35, "Input-only (ADC1), no internal pull-up — read-only signals such as MIDI_RX; cannot drive an output, and needs an external pull-up"));
    add(inputOnlyPin(36, "Input-only sensor VP (ADC1), no internal pull-up — read-only signals such as MIDI_RX; cannot output"));
    add(inputOnlyPin(39, "Input-only sensor VN (ADC1), no internal pull-up — read-only signals such as MIDI_RX; cannot output"));
}

}  // namespace

BoardProfile makeEsp32Wroom32() {
    BoardProfile b;
    b.identifier = "esp32-wroom-32";
    b.displayName = "ESP32-WROOM-32 (DevKitC, 38-pin)";
    addClassicEsp32Pins(b, /*includeFlash=*/true);
    return b;
}

BoardProfile makeEsp32DevKitV1() {
    BoardProfile b;
    b.identifier = "esp32-devkit-v1";
    b.displayName = "ESP32 DevKit v1 (30-pin)";
    addClassicEsp32Pins(b, /*includeFlash=*/false);
    return b;
}

// One list, built once, that every lookup and every enumeration goes through.
// Keeping the registry in a single place is what lets the web UI offer exactly
// the boards the firmware supports instead of a hand-maintained copy that drifts.
const std::vector<const BoardProfile*>& builtinBoardProfiles() {
    static const BoardProfile s3 = makeEsp32S3DevKitC1();
    static const BoardProfile s3v11 = makeEsp32S3DevKitC1V11();
    static const BoardProfile wroom = makeEsp32Wroom32();
    static const BoardProfile devkitv1 = makeEsp32DevKitV1();
    static const std::vector<const BoardProfile*> all{&s3, &s3v11, &wroom, &devkitv1};
    return all;
}

const BoardProfile* builtinBoardProfile(const std::string& identifier) {
    for (const BoardProfile* b : builtinBoardProfiles())
        if (identifier == b->identifier) return b;
    return nullptr;
}

}  // namespace gmb
