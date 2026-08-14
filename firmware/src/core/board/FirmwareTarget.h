// Which board is this BINARY for, and does the profile agree?
//
// The build already defined GMB_BOARD_ESP32S3_DEVKITC1 / GMB_BOARD_ESP32_WROOM32 /
// GMB_BOARD_ESP32_DEVKIT_V1 — and nothing read them. So the only thing that decided
// which GPIO map applied was `profile.boardIdentifier`, a field in a JSON file that
// travels between machines. A classic-ESP32 binary handed an S3 profile validated it
// against the S3 pin table and then configured those pins on hardware that does not
// have them.
//
// This closes that at the FAMILY level, which is the level that is physically real:
//
//   * The two S3 DevKitC-1 revisions run the same binary. They differ only in which
//     GPIO carries the on-board RGB LED, which the board PROFILE already handles.
//   * The WROOM-32 and DevKit v1 are the same classic ESP32 die on different
//     breakouts; the same binary runs on both, and the profile handles which pins
//     are broken out.
//   * S3 and classic ESP32 are NOT interchangeable: different GPIO ranges, different
//     peripheral counts, different USB.
//
// A build with NO macro (the Arduino IDE path, hostcheck, the native tests) reports
// `Unknown` and permits everything. That is deliberate: this check exists to catch a
// profile pointing at the wrong chip, not to break every build system that is not
// PlatformIO. `firmwareTargetKnown()` lets callers say so rather than imply a match
// they did not verify.
#pragma once

#include <string>

namespace gmb {

enum class BoardFamily : uint8_t {
    Unknown = 0,   // build did not declare a target — no restriction is enforced
    Esp32S3,
    Esp32Classic,
};

inline const char* boardFamilyName(BoardFamily f) {
    switch (f) {
        case BoardFamily::Esp32S3:      return "esp32s3";
        case BoardFamily::Esp32Classic: return "esp32";
        default:                        return "unknown";
    }
}

// The family this binary was compiled for.
constexpr BoardFamily compiledBoardFamily() {
#if defined(GMB_BOARD_ESP32S3_DEVKITC1)
    return BoardFamily::Esp32S3;
#elif defined(GMB_BOARD_ESP32_WROOM32) || defined(GMB_BOARD_ESP32_DEVKIT_V1)
    return BoardFamily::Esp32Classic;
#else
    return BoardFamily::Unknown;
#endif
}

constexpr bool firmwareTargetKnown() {
    return compiledBoardFamily() != BoardFamily::Unknown;
}

// Which family a board identifier belongs to. Kept as a pure function of the
// identifier (not of the BoardProfile) so it is total and host-testable: an
// identifier nobody recognises is Unknown, not a guess.
inline BoardFamily boardFamilyOf(const std::string& identifier) {
    if (identifier == "esp32-s3-devkitc-1" || identifier == "esp32-s3-devkitc-1-v1.1")
        return BoardFamily::Esp32S3;
    if (identifier == "esp32-wroom-32" || identifier == "esp32-devkit-v1")
        return BoardFamily::Esp32Classic;
    return BoardFamily::Unknown;
}

// May a profile declaring `identifier` run on this binary?
//
// Permissive in exactly two cases, both of them "we do not know", never "close
// enough": a build that declared no target, and an identifier this firmware does not
// recognise (which the board-profile lookup rejects on its own, with a better error).
inline bool boardMatchesFirmware(const std::string& identifier) {
    const BoardFamily target = compiledBoardFamily();
    if (target == BoardFamily::Unknown) return true;
    const BoardFamily want = boardFamilyOf(identifier);
    if (want == BoardFamily::Unknown) return true;
    return want == target;
}

}  // namespace gmb
