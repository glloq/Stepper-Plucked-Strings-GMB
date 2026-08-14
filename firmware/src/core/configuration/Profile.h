// Instrument configuration profile (spec section 20).
//
// This is the single source of truth for the firmware. The web UI edits a draft
// which is validated and then atomically activated; SysEx capabilities and the
// runtime are rebuilt from the active profile only.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../Types.h"
#include "../board/PinManager.h"
#include "../instrument/NoteAllocator.h"
#include "../midi/StringFretSelector.h"
#include "../motion/HomingController.h"
#include "../motion/StepperAxis.h"

namespace gmb {

// Current on-disk profile schema version. Bump it whenever the persisted shape
// changes and add a matching migrateV{N-1}ToV{N} step (see ProfileStorage::migrate)
// so old configs upgrade explicitly instead of accumulating special-cases in
// fromJson(). History: v1 → v2 dropped the no-op network.staticIp flag (audit P1.9).
constexpr uint16_t kCurrentProfileVersion = 2;

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
    // Announced polyphony (GMB descriptor / spec §6). 0 = automatic (= the number
    // of active, functional strings). A positive value caps it (e.g. a mechanical
    // constraint that only lets 4 of 6 strings sound); it is never announced above
    // the physical string count.
    uint8_t polyphonyMax = 0;
};

struct NetworkConfig {
    NetworkMode mode = NetworkMode::AccessPoint;
    std::string ssid;            // station SSID (never exported with password)
    std::string hostname = "gmb-instrument";
    std::string apSsid = "Stepper-Plucked-Strings-GMB";
    // NOTE: a `staticIp` flag was removed here (audit P1.9). It was persisted and
    // exposed in the UI but drove no WiFi.config() call — a phantom option. Real
    // static-IP support (ip/gateway/subnet/dns) belongs in the future DeviceConfig
    // (P1.13) and must arrive with the actual WiFi.config() wiring, not just a flag.
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
//   sharedDamper : one damper mechanism across several strings
//   aux    : any auxiliary actuator
// (Strumming is per string only — there is no shared strummer role.)
// (Function is kept as a string so the web UI can offer new roles without a
// firmware change.)
struct ServoConfig {
    bool enabled = false;
    std::string function = "finger";
    int8_t stringIndex = -1;      // owning string, or -1 for a shared/global servo

    // Signal source.
    ServoSource source = ServoSource::Pca;
    uint8_t pcaBoard = 0;         // 0..7 : PCA9685 address 0x40..0x47 within its bus
    // Which of the ESP32-S3's two hardware I2C controllers the board hangs off.
    // 0 = primary bus (Wire, SDA/SCL); 1 = secondary bus (Wire1, SDA2/SCL2). A board
    // is identified by (i2cBus, pcaBoard), so each bus has its own 0x40..0x47 range —
    // splitting the boards over two buses halves the traffic and refreshes faster.
    uint8_t i2cBus = 0;           // 0 | 1
    uint8_t channel = 0;          // PCA9685 channel 0..15 (source == Pca)
    int8_t gpio = -1;             // ESP32 GPIO           (source == DirectGpio)

    // Motion calibration (microseconds).
    uint16_t pulseMinUs = 500;
    uint16_t pulseMaxUs = 2500;
    uint16_t restUs = 1000;
    uint16_t activeUs = 1800;
    // Plectrum-as-mute position: the pulse at which a pluck/strum plectrum comes to
    // REST AGAINST the string to damp it at Note Off, so a string can be muted with
    // no dedicated damper servo (spec: "put the pluck servo against the string to
    // mute the note"). 0 = no mute position (the string rings / a damper is used).
    // Only meaningful on a pluck/strum servo; must sit in the pulse window when set.
    uint16_t muteUs = 0;
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

// Actuator current-draw management (audit P1.6). Two independent in-rush sources
// exist on this machine: a servo is heaviest in the first milliseconds of a move,
// and a stepper draws its peak when a whole chord's carriages accelerate together.
// A chord that re-frets many strings at once would stack those peaks and brown out
// the rail. The scheduler asks a governor for a "start permit" before each
// STAGGERABLE movement (finger press, strum-lift engage, carriage repositioning) so
// no more actuators begin moving together than the caps below allow, spaced by
// staggerMs. Sound-carrying strikes are never governed (see MoveClass::Deadline).
//
// The limit is OPTIONAL: a cap of 0 means "no limit" for that scope, so setting
// both caps to 0 turns the governor off entirely. The per-board cap is applied in
// ADDITION to the global one — each physical PCA9685 has its own power input, so a
// per-board cap bounds the in-rush on any single board even when the whole
// instrument is under the global cap.
struct PowerConfig {
    uint8_t maxConcurrentMoves = 3;    // global: actuators allowed to start at once (0 = no limit)
    uint8_t maxConcurrentPerBoard = 0; // per PCA9685 board: starts at once (0 = no limit)
    uint16_t staggerMs = 8;            // spacing between successive start permits
};

// Where a Note Off's damping comes from. `Auto` keeps the historical behaviour: a
// per-string `damper` servo mutes if one exists, otherwise the string is left to
// ring — so an absent PluckConfig changes nothing. `Plectrum` drives the string's
// own pluck/strum servo to its `muteUs` (rest against the string) with no dedicated
// damper. `Damper` forces the damper servo. `Lift` presses the strum-lift so the
// plectrum leans on the string. `None` never actively mutes (natural decay).
enum class MuteSource : uint8_t { Auto = 0, Plectrum = 1, Damper = 2, Lift = 3, None = 4 };

// How a strum lift engages the plectrum with the string — which end of the lift's
// travel plays and which end rests:
//   LowerToPlay : rest = plectrum CLEAR of the string; the lift LOWERS it onto the
//                 string to strike, then raises it back after (historical behaviour).
//   RaiseToPlay : rest = plectrum DOWN ON the string (so an idle string is muted);
//                 the lift RAISES it to play, HOLDS it up for the whole note, then
//                 lets it fall back onto the string at Note Off — the lift is the
//                 damper, so no separate mute is needed on a lifted string.
enum class LiftEngage : uint8_t { LowerToPlay = 0, RaiseToPlay = 1 };

// Plucking ("grattage") settings that are COMMON to every string, so the whole
// gesture and its timing are set in one place instead of servo by servo. Every
// field defaults to the historical behaviour, so a profile with no `pluck` block
// plays exactly as before:
//   strokeMs       : global stroke engage time; 0 = each servo keeps its own strokeMs.
//   minStrikePct   : global minimum strike depth as a PERCENTAGE of each striker's
//                    rest->active span, so a soft (low-velocity) note still catches
//                    the string. Servo-independent (unlike the per-servo minStrikeUs);
//                    0 = each servo keeps its own minStrikeUs.
//   fretToPluckMs  : extra settle inserted BETWEEN the fret being ready and the
//                    strike, on top of the finger's settleMs — the "delay between
//                    fret action and plucking". 0 = none.
//   muteSource     : who damps at Note Off (see MuteSource); Auto = historical.
//   muteHoldMs     : how long the mute stays engaged before releasing to rest.
//   liftMuteOnNoteOff : also press the strum-lift at Note Off so the plectrum leans
//                    on the string (a lift that doubles as a damper).
// The fixed Note-On->sound latency and the anticipation leads stay in MidiConfig
// (noteExecutionDelayMs / fingerLeadMs / strumLeadMs); this block adds the gesture
// and the mute behaviour that had no home.
struct PluckConfig {
    uint16_t strokeMs = 0;
    uint8_t minStrikePct = 0;
    uint16_t fretToPluckMs = 0;
    MuteSource muteSource = MuteSource::Auto;
    uint16_t muteHoldMs = 60;
    bool liftMuteOnNoteOff = false;
    LiftEngage liftEngage = LiftEngage::LowerToPlay;
};

// Documentation of the PHYSICAL power & safety hardware built around the
// electronics (web interface: Wiring → "Power & safety" and "I²C & PCA" tabs;
// reference circuit: hardware/POWER_AND_SAFETY.md). The firmware stores and
// round-trips this block so the builder's declarations follow the profile across
// devices and exports — it drives NO runtime behaviour: the hardware it
// describes (contactor, /OE pull-up, fuses, driver ENABLE gating) matters
// precisely when the firmware cannot act.
struct PcaPullupNote {
    uint8_t i2cBus = 0;    // 0 | 1
    uint8_t board = 0;     // address index 0..7 (I2C address = 0x40 + board)
    uint32_t ohm = 10000;  // on-board SDA/SCL pull-up value; 0 = removed/absent
};
struct HardwareNotes {
    bool oePullup = false;        // external /OE pull-up to 3.3 V fitted (mandatory)
    bool oeGate = false;          // gated /OE enable stage (E-stop can inhibit enabling)
    bool estopCutsPower = false;  // E-stop chain drops the motor-rail contactor (K1)
    bool estopCutsDriverEnable = false;  // E-stop also forces the STEP/DIR driver ENABLE inactive
    bool mainSwitch = false;      // master switch (S1) fitted upstream of K1
    bool mainFuse = false;        // main motor-rail fuse (F0) fitted
    bool branchFuses = false;     // one fuse per PCA/driver/direct branch (F1..Fn) fitted
    // Fleet-typical per-servo currents (mA) used by the web power estimator.
    uint16_t servoIdleMa = 10;
    uint16_t servoMoveMa = 250;
    uint16_t servoStallMa = 800;
    // Per-axis stepper currents (mA) used by the same estimator. A STEP/DIR driver
    // draws its programmed phase current whenever the coils are energised, so the
    // idle (holding) figure matters as much as the moving one.
    uint16_t stepperHoldMa = 400;
    uint16_t stepperMoveMa = 800;
    // External I2C pull-up pair per bus, in ohms (0 = none fitted).
    uint32_t extPullupOhm0 = 0;
    uint32_t extPullupOhm1 = 0;
    std::vector<PcaPullupNote> pcaPullups;  // per-board on-board pull-up notes
};

struct Profile {
    std::string project = "Stepper-Plucked-Strings-GMB";
    uint16_t profileVersion = kCurrentProfileVersion;
    uint32_t capabilitiesRevision = 1;

    InstrumentInfo instrument;
    std::string boardIdentifier = "esp32-s3-devkitc-1";
    bool reserveUsb = true;
    bool automaticPinAssignment = true;
    // How the hardware E-stop contact on the `ESTOP` pin is wired (only meaningful
    // when an ESTOP pin is assigned). false = legacy normally-open button to GND
    // (active-low: LOW = stop). true = NORMALLY-CLOSED loop to GND — the RECOMMENDED
    // wiring (hardware/POWER_AND_SAFETY.md): the closed loop holds the pin LOW to
    // authorise running, and a press, a broken wire or an unplugged connector all
    // read HIGH (pull-up) = stop, so losing the E-stop chain fails safe. Defaults to
    // the legacy polarity so existing wired instruments keep their behaviour.
    bool estopNormallyClosed = false;
    std::vector<PinAssignment> pins;

    NetworkConfig network;
    MidiConfig midi;
    SelectorConfig selector;
    PowerConfig power;
    PluckConfig pluck;
    HardwareNotes hardware;

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
