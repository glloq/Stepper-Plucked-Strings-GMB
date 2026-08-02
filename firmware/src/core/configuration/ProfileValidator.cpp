#include "ProfileValidator.h"

#include <utility>

namespace gmb {

using Sev = ValidationIssue::Severity;

std::vector<ValidationIssue> ProfileValidator::validate(const Profile& p) {
    std::vector<ValidationIssue> issues;
    auto err = [&](const std::string& f, const std::string& m) {
        issues.push_back({Sev::Error, f, m});
    };
    auto warn = [&](const std::string& f, const std::string& m) {
        issues.push_back({Sev::Warning, f, m});
    };

    // Capacity: 1..6 strings, one stepper axis and one finger per string.
    if (p.instrument.stringCount < 1 || p.instrument.stringCount > kMaxStrings) {
        err("instrument.stringCount", "String count must be between 1 and 6");
    }
    if (p.strings.size() != p.instrument.stringCount) {
        err("strings", "Number of configured axes must equal the string count");
    }
    if (p.homing.size() != p.strings.size()) {
        err("homing", "Exactly one homing configuration is required per axis");
    }

    // Mandatory mechanical pins must be present, not merely conflict-free.
    auto hasPin = [&](const std::string& sig) {
        for (const auto& a : p.pins)
            if (a.signal == sig && a.gpio >= 0) return true;
        return false;
    };
    bool anyPca = false;
    int directServos = 0;
    for (const auto& s : p.servos) {
        if (!s.enabled) continue;
        if (s.source == ServoSource::Pca) anyPca = true;
        else ++directServos;
    }
    for (size_t i = 0; i < p.strings.size(); ++i) {
        if (!p.strings[i].enabled) continue;  // a disabled axis needs no pins
        std::string n = std::to_string(i + 1);
        if (!hasPin("STEP" + n)) err("pins.STEP" + n, "STEP pin is required for this axis");
        if (!hasPin("DIR" + n)) err("pins.DIR" + n, "DIR pin is required for this axis");
        if (!hasPin("HOME" + n)) err("pins.HOME" + n, "HOME endstop pin is required for this axis");
    }
    if (!p.strings.empty() && !hasPin("ENABLE"))
        err("pins.ENABLE", "A driver ENABLE pin is required");
    if (anyPca) {
        if (!hasPin("SDA") || !hasPin("SCL"))
            err("pins.i2c", "SDA and SCL are required when a PCA9685 is used");
        if (!hasPin("SERVO_OE"))
            err("pins.SERVO_OE", "The PCA9685 /OE safety pin is required");
    }
    // ESP32-S3 has 8 LEDC channels; a direct servo consumes one.
    if (directServos > 8)
        err("servos.direct",
            "At most 8 direct-GPIO servos are supported (ESP32-S3 has 8 LEDC channels)");

    // Per-string sanity.
    for (size_t i = 0; i < p.strings.size(); ++i) {
        const AxisConfig& a = p.strings[i];
        std::string t = "strings[" + std::to_string(i) + "]";
        if (a.maxPositionMm <= a.minPositionMm) {
            err(t + ".limits", "Maximum position must be greater than minimum position");
        }
        if (a.scaleLengthMm <= 0.0) {
            err(t + ".scaleLength", "Scale length must be positive");
        }
        // Numeric drive parameters.
        if (a.microsteps == 0) err(t + ".microsteps", "Microstepping must be > 0");
        if (a.stepsPerRevolution == 0)
            err(t + ".stepsPerRevolution", "Steps per revolution must be > 0");
        if (a.maxSpeedMmS <= 0.0) err(t + ".maxSpeedMmS", "Max speed must be > 0");
        if (a.maxAccelMmS2 <= 0.0) err(t + ".maxAccelMmS2", "Max acceleration must be > 0");

        // Transmission-specific parameters must be physically valid for the
        // SELECTED type, otherwise stepsPerMm() silently falls back to the custom
        // ratio and the axis moves the wrong distance.
        switch (a.transmission) {
            case Transmission::BeltGt2:
                if (a.pulleyTeeth == 0)
                    err(t + ".pulleyTeeth", "Belt: pulley teeth must be > 0");
                if (a.beltPitchMm <= 0.0)
                    err(t + ".beltPitchMm", "Belt: pitch must be > 0");
                break;
            case Transmission::Screw:
                if (a.leadPerRevolutionMm <= 0.0)
                    err(t + ".leadPerRevolutionMm", "Screw: lead per revolution must be > 0");
                break;
            case Transmission::Custom:
                if (a.customStepsPerMm <= 0.0)
                    err(t + ".customStepsPerMm", "Custom: steps/mm must be > 0");
                break;
        }

        // Ensure the calculated span physically fits.
        double lastFret = gmb::fretPositionMm(a.scaleLengthMm, a.maxFret);
        if (lastFret > a.maxPositionMm - a.minPositionMm + 0.001) {
            warn(t + ".travel", "Highest fret position exceeds the configured travel");
        }

        // Calibrated fret table must be monotonic and inside the travel.
        double prev = -1e9;
        for (size_t f = 0; f < a.calibratedFretMm.size(); ++f) {
            double v = a.calibratedFretMm[f];
            if (v < a.minPositionMm - 1e-6 || v > a.maxPositionMm + 1e-6)
                err(t + ".calibratedFretMm[" + std::to_string(f) + "]",
                    "Calibrated fret position is outside the axis travel");
            if (v < prev - 1e-6)
                err(t + ".calibratedFretMm[" + std::to_string(f) + "]",
                    "Calibrated fret positions must be non-decreasing");
            prev = v;
        }

        // Homing sanity for this axis.
        if (i < p.homing.size()) {
            const HomingConfig& h = p.homing[i];
            if (h.direction != 1 && h.direction != -1)
                err(t + ".homing.direction", "Homing direction must be +1 or -1");
            if (h.fastSpeedMmS <= 0.0 || h.slowSpeedMmS <= 0.0)
                err(t + ".homing.speed", "Homing speeds must be > 0");
            if (h.timeoutMs == 0) err(t + ".homing.timeout", "Homing timeout must be > 0");
            if (h.maxSearchMm <= 0.0)
                err(t + ".homing.maxSearch", "Homing max search distance must be > 0");
        }
    }

    // Pin validation on the reference board.
    if (const BoardProfile* board = builtinBoardProfile(p.boardIdentifier)) {
        PinManager pm(*board);
        pm.set(p.pins);
        for (const auto& e : pm.validate(p.reserveUsb)) {
            std::string msg = e.reason;
            if (!e.conflictWith.empty()) msg += " (used by " + e.conflictWith + ")";
            if (!e.suggestion.empty()) msg += " — " + e.suggestion;
            err("pins." + e.signal, msg);
        }
    } else {
        warn("board", "Unknown board profile; pins cannot be validated");
    }

    // Servo configuration: PCA vs direct GPIO, with or without a PCA9685.
    {
        const BoardProfile* board = builtinBoardProfile(p.boardIdentifier);
        // Collect stepper/other GPIOs already used so direct servos can't clash.
        std::vector<std::pair<int8_t, std::string>> usedGpio;
        for (const auto& a : p.pins)
            if (a.gpio >= 0) usedGpio.push_back({a.gpio, a.signal});

        std::vector<std::pair<int, int>> usedPcaChannels;  // (board, channel)
        for (size_t i = 0; i < p.servos.size(); ++i) {
            const ServoConfig& s = p.servos[i];
            if (!s.enabled) continue;
            std::string tag = "servos[" + std::to_string(i) + "]";

            // Per-string roles must reference an existing string.
            bool perString = s.function == "finger" || s.function == "pluck" ||
                             s.function == "strum" || s.function == "damper";
            if (perString) {
                if (s.stringIndex < 0 || s.stringIndex >= (int)p.strings.size())
                    err(tag + ".stringIndex",
                        "Per-string servo references a string that does not exist");
            }

            if (s.pulseMinUs >= s.pulseMaxUs)
                err(tag + ".pulse", "pulseMinUs must be less than pulseMaxUs");
            if (s.restUs < s.pulseMinUs || s.restUs > s.pulseMaxUs)
                err(tag + ".restUs", "Rest pulse is outside the servo's min/max range");
            if (s.activeUs < s.pulseMinUs || s.activeUs > s.pulseMaxUs)
                err(tag + ".activeUs", "Active pulse is outside the servo's min/max range");

            if (s.source == ServoSource::Pca) {
                if (s.pcaBoard > 3)
                    err(tag + ".pcaBoard", "PCA board index must be 0..3 (max four PCA9685)");
                if (s.channel > 15)
                    err(tag + ".channel", "PCA channel must be 0..15");
                std::pair<int, int> key{s.pcaBoard, s.channel};
                for (auto& u : usedPcaChannels)
                    if (u == key)
                        err(tag + ".channel",
                            "PCA board " + std::to_string(s.pcaBoard) + " channel " +
                                std::to_string(s.channel) + " is already used by another servo");
                usedPcaChannels.push_back(key);
            } else {  // DirectGpio
                if (s.gpio < 0) {
                    err(tag + ".gpio", "Direct servo requires a GPIO");
                } else if (board && !board->supports(s.gpio, SignalKind::Generic)) {
                    err(tag + ".gpio",
                        "GPIO " + std::to_string(s.gpio) +
                            " cannot drive a servo on this board (reserved or output-incapable)");
                }
                for (auto& u : usedGpio)
                    if (u.first == s.gpio)
                        err(tag + ".gpio", "GPIO " + std::to_string(s.gpio) +
                                               " already used by " + u.second);
                if (s.gpio >= 0) usedGpio.push_back({s.gpio, tag});
            }
        }
    }

    // Pluck-mode servo presence (cahier des charges §5.3).
    {
        auto hasServoRole = [&](const std::string& fn, int strIdx) {
            for (const auto& s : p.servos)
                if (s.enabled && s.function == fn && s.stringIndex == strIdx) return true;
            return false;
        };
        bool needIndividual = p.instrument.pluckMode == PluckMode::Individual ||
                              p.instrument.pluckMode == PluckMode::Both;
        bool needShared = p.instrument.pluckMode == PluckMode::SharedStrum ||
                          p.instrument.pluckMode == PluckMode::Both;
        if (needIndividual) {
            for (size_t i = 0; i < p.strings.size(); ++i)
                if (p.strings[i].enabled &&
                    !hasServoRole("pluck", static_cast<int>(i)))
                    err("servos.pluck",
                        "String " + std::to_string(i) +
                            " needs a pluck servo for the individual pluck mode");
        }
        if (needShared) {
            bool found = false;
            for (const auto& s : p.servos)
                if (s.enabled && s.function == "sharedStrum") found = true;
            if (!found)
                err("servos.sharedStrum",
                    "A sharedStrum servo is required for the shared-strum pluck mode");
        }
    }

    // String/fret selection CC configuration (selection spec section 18).
    const SelectorConfig& s = p.selector;
    if (s.string.ccNumber > kMaxAssignableCc)
        err("selector.string.cc", "String CC must be 0..119 (120..127 are mode messages)");
    if (s.fret.ccNumber > kMaxAssignableCc)
        err("selector.fret.cc", "Fret CC must be 0..119 (120..127 are mode messages)");
    if (s.string.ccNumber == s.fret.ccNumber)
        err("selector.cc", "String and fret CC numbers must differ");
    if (s.selectionTimeoutMs < 5 || s.selectionTimeoutMs > 2000)
        err("selector.timeout", "Selection timeout must be 5..2000 ms");
    if (s.queueDepth < 16 || s.queueDepth > 256)
        err("selector.queueDepth", "Selection queue depth must be 16..256");
    if (s.string.minimum > s.string.maximum)
        err("selector.string.range", "String CC minimum must be <= maximum");
    if (s.fret.minimum > s.fret.maximum)
        err("selector.fret.range", "Fret CC minimum must be <= maximum");
    // Custom string mapping, when present, must be one entry per string and each
    // must reference a valid axis.
    if (!s.string.mapping.empty()) {
        if (s.string.mapping.size() != p.strings.size())
            err("selector.string.mapping", "Mapping must have one entry per string");
        for (int8_t m : s.string.mapping)
            if (m < 0 || m >= static_cast<int>(p.strings.size()))
                err("selector.string.mapping", "Mapping references a non-existent axis");
    }

    // MIDI ranges.
    if (p.midi.sustainCc > kMaxAssignableCc)
        err("midi.sustainCc", "Sustain CC must be 0..119");
    if (p.instrument.capo < 0 || p.instrument.capo > 24)
        err("instrument.capo", "Capo must be 0..24");
    if (p.instrument.transpose < -48 || p.instrument.transpose > 48)
        err("instrument.transpose", "Transpose must be within +/-48 semitones");
    // Homing back-off must be non-negative.
    for (size_t i = 0; i < p.homing.size(); ++i)
        if (p.homing[i].backoffMm < 0.0)
            err("strings[" + std::to_string(i) + "].homing.backoff",
                "Homing back-off distance must be >= 0");

    // Fret CC max should not exceed the instrument's playable frets.
    uint8_t maxFret = 0;
    for (const auto& a : p.strings) maxFret = a.maxFret > maxFret ? a.maxFret : maxFret;
    if (s.fret.maximum > maxFret)
        warn("selector.fret.maximum", "Fret CC maximum exceeds the instrument's frets");
    if (s.string.maximum > p.strings.size())
        warn("selector.string.maximum", "String CC maximum exceeds the string count");

    // MIDI.
    if (!p.midi.omni && p.midi.globalChannel > 15)
        err("midi.channel", "MIDI channel must be 0..15");

    // Profiles requirement.
    if (p.capabilitiesRevision == 0)
        warn("capabilitiesRevision", "Revision counter should start at 1");

    return issues;
}

}  // namespace gmb
