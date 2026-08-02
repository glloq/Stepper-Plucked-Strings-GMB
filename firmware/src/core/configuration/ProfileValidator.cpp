#include "ProfileValidator.h"

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
    if (!p.homing.empty() && p.homing.size() != p.strings.size()) {
        err("homing", "One homing configuration is required per axis");
    }

    // Per-string sanity.
    for (size_t i = 0; i < p.strings.size(); ++i) {
        const AxisConfig& a = p.strings[i];
        if (a.maxPositionMm <= a.minPositionMm) {
            err("strings[" + std::to_string(i) + "].limits",
                "Maximum position must be greater than minimum position");
        }
        if (a.scaleLengthMm <= 0.0) {
            err("strings[" + std::to_string(i) + "].scaleLength",
                "Scale length must be positive");
        }
        // Ensure the calculated span physically fits.
        double lastFret = gmb::fretPositionMm(a.scaleLengthMm, a.maxFret);
        if (lastFret > a.maxPositionMm - a.minPositionMm + 0.001) {
            warn("strings[" + std::to_string(i) + "].travel",
                 "Highest fret position exceeds the configured travel");
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
    if (s.queueDepth < 16)
        err("selector.queueDepth", "Selection queue must hold at least 16 entries");

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
