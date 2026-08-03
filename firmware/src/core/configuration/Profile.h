// Instrument configuration profile (spec section 20).
//
// This is the single source of truth for the firmware. The web UI edits a draft
// which is validated and then atomically activated; SysEx capabilities and the
// runtime are rebuilt from the active profile only.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../board/PinManager.h"
#include "../instrument/NoteAllocator.h"
#include "../midi/StringFretSelector.h"
#include "../motion/HomingController.h"
#include "../motion/StepperAxis.h"

namespace gmb {

enum class PluckMode : uint8_t { Individual = 0, SharedStrum = 1, Both = 2 };
enum class NetworkMode : uint8_t { AccessPoint = 0, Station = 1 };

enum class VelocityCurve : uint8_t { Linear, Soft, Hard, Exponential, Custom };

struct InstrumentInfo {
    std::string name = "Instrument";
    std::string description;
    uint8_t stringCount = 4;
    std::string type = "guitar";
    uint8_t gmProgram = 24;   // GM 24 = nylon guitar
    uint8_t typeId = 0x04;    // GMB instrument type id (guitar)
    int8_t capo = 0;
    int8_t transpose = 0;
    PluckMode pluckMode = PluckMode::Individual;
};

struct NetworkConfig {
    NetworkMode mode = NetworkMode::AccessPoint;
    std::string ssid;            // station SSID (never exported with password)
    std::string hostname = "gmb-instrument";
    std::string apSsid = "Stepper-Plucked-Strings-GMB";
    bool staticIp = false;
};

struct MidiConfig {
    uint8_t globalChannel = 0;   // zero-based internal channel
    bool omni = false;
    int8_t transpose = 0;
    uint8_t chordWindowMs = 3;   // grouping window (spec 17.2)
    VelocityCurve velocityCurve = VelocityCurve::Linear;
    bool sustainPedal = true;
    uint8_t sustainCc = 64;
    SaturationStrategy saturationStrategy = SaturationStrategy::PriorityLow;

    // Playback timing / latency management.
    //   noteExecutionDelayMs : fixed delay between receiving a Note On and the
    //                          note actually sounding, so the mechanics have a
    //                          predictable, constant window to get in position.
    //   fingerLeadMs         : begin the finger descent up to this long before
    //                          the carriage is estimated to reach the fret, so
    //                          the finger arrives on the string around the same
    //                          time (overlaps descent with the approach).
    //   strumLeadMs          : begin lowering the strum lift up to this long
    //                          before the string is ready, so the strummer is
    //                          already engaged when the strike time comes.
    // The two leads shrink the minimum achievable noteExecutionDelayMs; both
    // default to 0 (no anticipation — the safe, strictly-sequential behaviour).
    uint16_t noteExecutionDelayMs = 0;
    uint16_t fingerLeadMs = 0;
    uint16_t strumLeadMs = 0;
};

// Where a servo's PWM signal comes from. The system must work with OR without a
// PCA9685 (a servo can hang directly off a free ESP32 GPIO), and both can be
// mixed on the same instrument.
enum class ServoSource : uint8_t { Pca = 0, DirectGpio = 1 };

// Servo roles. Per-string roles carry a stringIndex; shared roles use -1.
//   finger : presses the string at the fret            (per string)
//   pluck  : individual plectrum                       (per string)
//   strum  : per-string strum servo                    (per string)
//   strumLift : lowers/raises the strum servo per stroke (per string)
//   damper : per-string damper (mute)                  (per string)
//   sharedStrum / sharedDamper : one mechanism across several strings
//   aux    : any auxiliary actuator
// (Function is kept as a string so the web UI can offer new roles without a
// firmware change.)
struct ServoConfig {
    bool enabled = false;
    std::string function = "finger";
    int8_t stringIndex = -1;      // owning string, or -1 for a shared/global servo

    // Signal source.
    ServoSource source = ServoSource::Pca;
    uint8_t pcaBoard = 0;         // 0..3 : up to four PCA9685 (0x40..0x43)
    uint8_t channel = 0;          // PCA9685 channel 0..15 (source == Pca)
    int8_t gpio = -1;             // ESP32 GPIO           (source == DirectGpio)

    // Motion calibration (microseconds).
    uint16_t pulseMinUs = 500;
    uint16_t pulseMaxUs = 2500;
    uint16_t restUs = 1000;
    uint16_t activeUs = 1800;
    bool inverted = false;
    uint16_t travelMs = 120;
    uint16_t settleMs = 30;
    bool disableAtRest = true;

    // Strum / pluck stroke shaping.
    //   engageDelayMs      : for a strumLift, extra pause after the lift is down,
    //                        before the strum stroke fires (0 = none).
    //   alternateDirection : alternate the stroke endpoint on successive strikes
    //                        (down-stroke, up-stroke, down-stroke…).
    //   activeAltUs        : the up-stroke active pulse (0 = mirror activeUs about
    //                        restUs). Only used when alternateDirection is set.
    //   strokeMs           : time the stroke stays engaged before it returns to
    //                        rest (0 = use travelMs). Lets the stroke "speed" be
    //                        set independently of the return/settle timing.
    //   minStrikeUs        : guaranteed minimum strike depth toward the active side
    //                        so a low-velocity note still catches the string
    //                        (0 = disabled, depth follows velocity only).
    uint16_t engageDelayMs = 0;
    bool alternateDirection = false;
    uint16_t activeAltUs = 0;
    uint16_t strokeMs = 0;
    uint16_t minStrikeUs = 0;
};

struct Profile {
    std::string project = "Stepper-Plucked-Strings-GMB";
    uint16_t profileVersion = 1;
    uint32_t capabilitiesRevision = 1;

    InstrumentInfo instrument;
    std::string boardIdentifier = "esp32-s3-devkitc-1";
    bool reserveUsb = true;
    bool automaticPinAssignment = true;
    std::vector<PinAssignment> pins;

    NetworkConfig network;
    MidiConfig midi;
    SelectorConfig selector;

    std::vector<AxisConfig> strings;
    std::vector<HomingConfig> homing;
    std::vector<ServoConfig> servos;

    // Build an InstrumentView (used by the selector and capabilities) from the
    // string list.
    InstrumentView instrumentView() const;

    // Convenience: create a sensible default profile for a given instrument.
    static Profile makeDefault(const std::string& name, uint8_t stringCount,
                               const std::vector<uint8_t>& tuning, uint8_t maxFret);
};

}  // namespace gmb
