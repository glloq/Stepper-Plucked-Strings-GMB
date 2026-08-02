#include "PinManager.h"

#include <string>

namespace gmb {

bool PinManager::isUsed(int8_t gpio, const std::string& exceptSignal) const {
    for (const auto& a : assignments_) {
        if (a.signal != exceptSignal && a.gpio == gpio) return true;
    }
    return false;
}

int8_t PinManager::gpioOf(const std::string& signal) const {
    for (const auto& a : assignments_) {
        if (a.signal == signal) return a.gpio;
    }
    return kNoPin;
}

void PinManager::assign(const std::string& signal, SignalKind kind, int8_t gpio) {
    for (auto& a : assignments_) {
        if (a.signal == signal) {
            a.kind = kind;
            a.gpio = gpio;
            return;
        }
    }
    assignments_.push_back({signal, kind, gpio});
}

bool PinManager::place(const std::string& signal, SignalKind kind,
                       const std::vector<int8_t>& preferred) {
    // Try the recommended pins first.
    for (int8_t gpio : preferred) {
        if (board_.supports(gpio, kind) && !isUsed(gpio)) {
            assignments_.push_back({signal, kind, gpio});
            return true;
        }
    }
    // Fall back to any compatible, unused candidate.
    for (const PinCapability* c : board_.candidatesFor(kind)) {
        if (!isUsed(c->gpio)) {
            assignments_.push_back({signal, kind, c->gpio});
            return true;
        }
    }
    return false;
}

bool PinManager::autoAssign(const PinRequest& req) {
    clear();
    bool ok = true;
    const int n = static_cast<int>(clampValue<int>(req.stringCount, 1, kMaxStrings));

    // Recommended assignment table (cahier des charges 11.5).
    const std::vector<int8_t> stepPref = {4, 5, 6, 7, 15, 16};
    const std::vector<int8_t> dirPref = {17, 18, 8, 9, 10, 11};
    const std::vector<int8_t> homePref = {12, 13, 14, 21, 38, 39};

    for (int i = 0; i < n; ++i) {
        ok &= place("STEP" + std::to_string(i + 1), SignalKind::Step,
                    {stepPref[i]});
        ok &= place("DIR" + std::to_string(i + 1), SignalKind::Dir, {dirPref[i]});
        ok &= place("HOME" + std::to_string(i + 1), SignalKind::Home, {homePref[i]});
        if (req.useLimitSwitches) {
            ok &= place("LIMIT" + std::to_string(i + 1), SignalKind::Limit, {});
        }
    }

    if (req.globalEnable) {
        ok &= place("ENABLE", SignalKind::Enable, {42});
    }
    if (req.useI2cServos) {
        ok &= place("SDA", SignalKind::I2cSda, {40});
        ok &= place("SCL", SignalKind::I2cScl, {41});
    }
    if (req.servoSafetyOe) {
        ok &= place("SERVO_OE", SignalKind::ServoOe, {47});
    }
    return ok;
}

std::vector<PinError> PinManager::validate(bool reserveUsb) const {
    std::vector<PinError> errors;

    for (const auto& a : assignments_) {
        const PinCapability* cap = board_.find(a.gpio);

        // Unknown / not exposed on this board.
        if (cap == nullptr || !cap->exposed) {
            errors.push_back({a.signal, a.gpio,
                              "GPIO not available on this board variant",
                              "Pick a pin listed for this board", ""});
            continue;
        }

        // USB reservation (cahier des charges 11.3).
        if (reserveUsb && cap->usb) {
            errors.push_back({a.signal, a.gpio,
                              "Reserved for future native USB (GPIO19/20)",
                              "Choose another output pin", ""});
        }

        // Reserved / Flash / PSRAM / strapping / on-board peripheral.
        if (cap->reserved) {
            std::string why = cap->note.empty() ? "Pin is reserved" : cap->note;
            errors.push_back({a.signal, a.gpio, why,
                              "Choose a recommended (green) pin", ""});
        }

        // Signal / capability mismatch.
        if (!board_.supports(a.gpio, a.kind)) {
            std::string why = "Pin cannot carry this signal type";
            if (a.kind == SignalKind::Step) {
                why = "Pin is not suitable for a high-speed STEP output";
            } else if (a.kind == SignalKind::Home || a.kind == SignalKind::Limit) {
                why = "Pin cannot be used as an interrupt-capable input";
            }
            errors.push_back({a.signal, a.gpio, why,
                              "Pick a pin compatible with this function", ""});
        }
    }

    // Duplicate GPIO detection (two signals on the same pin).
    for (size_t i = 0; i < assignments_.size(); ++i) {
        if (assignments_[i].gpio < 0) continue;
        for (size_t j = i + 1; j < assignments_.size(); ++j) {
            if (assignments_[i].gpio == assignments_[j].gpio) {
                errors.push_back({assignments_[j].signal, assignments_[j].gpio,
                                  "GPIO already used by another signal",
                                  "Assign a different free pin",
                                  assignments_[i].signal});
            }
        }
    }

    return errors;
}

}  // namespace gmb
