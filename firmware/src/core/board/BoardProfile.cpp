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
            return p.input && p.interrupt;
        case SignalKind::Diag:
            return p.input;
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

}  // namespace

BoardProfile makeEsp32S3DevKitC1() {
    BoardProfile b;
    b.identifier = "esp32-s3-devkitc-1";
    b.displayName = "ESP32-S3-DevKitC-1";

    auto add = [&](PinCapability c) { b.pins.push_back(c); };

    // ADC1 covers GPIO1..10, ADC2 covers GPIO11..20 on the ESP32-S3.
    // Strapping / boot pins (cahier des charges 11.4).
    add(reservedPin(0, "Strapping / BOOT button", /*strapping=*/true));
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
    add(reservedPin(48, "On-board RGB LED", false, false, true));

    return b;
}

const BoardProfile* builtinBoardProfile(const std::string& identifier) {
    static const BoardProfile devkit = makeEsp32S3DevKitC1();
    if (identifier == devkit.identifier) return &devkit;
    return nullptr;
}

}  // namespace gmb
