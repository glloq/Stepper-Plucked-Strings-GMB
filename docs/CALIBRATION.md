# Calibration procedure — Stepper-Plucked-Strings-GMB

> Sources: `SPECIFICATION.md` §12, §13, §14, §15 · Code: `core/motion/{StepperAxis.*, HomingController.*}`, `core/Types.*`, `core/configuration/Profile.h`.
> Related documents: [`WEB_INTERFACE.md`](WEB_INTERFACE.md) (wizard §10) · [`SAFETY.md`](SAFETY.md) · [`FIRST_CONFIGURATION.md`](FIRST_CONFIGURATION.md).

This document describes calibration: motor steps/mm, homing, fret positions
(theoretical and manual), and servos.

---

## 1. Assisted steps/mm calculation (§12.1)

The firmware works in **millimeters** and converts to motor steps via a
`steps/mm` factor that depends on the transmission (`StepperAxis::stepsPerMm()`),
which abstracts the mechanical type away from the rest of the code.

```cpp
enum class Transmission { BeltGt2, Screw, Custom };
```

### 1.1 Belt (GT2)

```text
                stepsPerRevolution × microsteps
stepsPerMm = ──────────────────────────────────
                    pulleyTeeth × beltPitch
```

Example: 1.8° motor (200 steps/rev), 16 microsteps, 20-tooth pulley, GT2 pitch
2 mm → `(200 × 16) / (20 × 2) = 80 steps/mm`.

### 1.2 Screw

```text
                stepsPerRevolution × microsteps
stepsPerMm = ──────────────────────────────────
                    leadPerRevolution
```

Example: 200 steps/rev, 16 microsteps, screw with 8 mm/rev lead → `3200 / 8 = 400 steps/mm`.

### 1.3 Custom value

`Custom` transmission: `customStepsPerMm` is used directly.

Relevant parameters in `AxisConfig`: `stepsPerRevolution`, `microsteps`,
`pulleyTeeth`, `beltPitchMm`, `leadPerRevolutionMm`, `customStepsPerMm`,
`invertDirection`, `minPositionMm`/`maxPositionMm`, `maxSpeedMmS`, `maxAccelMmS2`.
Conversions: `mmToSteps(mm)`, `stepsToMm(steps)`, clamping `clampToLimits(mm)`.

---

## 2. Homing (§13)

Homing is **non-blocking** and **independent** for each string
(`HomingController`, one instance per axis). On each tick it reads the sensor and
the position, and returns the motion command to apply (`HomingCommand`).

### 2.1 State machine

```text
Idle → CheckSensor → SeekFast → (SensorDetected) → Backoff →
SeekSlow → SetZero → MoveToOffset → Ready
                                       └─(fault)─► Fault
```

| State (`HomingState`) | Role |
| -------------------- | ---- |
| `Idle` | inactive |
| `CheckSensor` | verify the sensor is not already active |
| `SeekFast` | fast approach toward the sensor (`fastSpeedMmS`) |
| `Backoff` | back off after detection (`backoffMm`) |
| `SeekSlow` | slow, precise re-approach (`slowSpeedMmS`) |
| `SetZero` | set the origin |
| `MoveToOffset` | move to the rest offset (`offsetMm`) |
| `Ready` | axis ready |
| `Fault` | axis disabled |

Configuration (`HomingConfig`): `direction` (±1 toward the sensor), `fastSpeedMmS`,
`slowSpeedMmS`, `backoffMm`, `offsetMm`, `timeoutMs` (default 8000), `maxSearchMm`
(default 500), `sensorActiveHigh` (the raw electrical level is normalized
internally).

### 2.2 Detected faults (§13.2, `HomingFault`)

| Fault | Cause |
| ------ | ----- |
| `SensorActiveAtStart` | sensor active at startup |
| `SensorNotReleased` | sensor cannot be released |
| `SensorNeverReached` | sensor never reached |
| `Timeout` | timeout exceeded |
| `MaxDistanceExceeded` | maximum search distance exceeded |
| — | inconsistent activation of HOME and LIMIT (detected upstream) |

A faulty axis is disabled **without causing any unexpected movement** on the
other axes.

### 2.3 Parallel homing (§13.1)

Options: **simultaneous**, **sequential** (if power supply is limited), or **by
groups**. Each axis keeps its own `HomingController` instance.

---

## 3. Note / fret calibration (§14)

### 3.1 Tuning

Each string has: open MIDI note (`openNote`), maximum fret included (`maxFret`),
and the position of each fret. Predefined tunings are provided (guitar, bass,
ukulele, mandolin, banjo, custom), all fully editable.

### 3.2 Theoretical calculation (§14.2)

```text
position = scale length × (1 − 2^(−fret / 12))
```

Implemented in `core/Types.cpp` (`fretPositionMm(scaleLengthMm, fret)`) and
exposed by `StepperAxis::fretPositionMm(fret)`. `scaleLengthMm` = the string's
vibrating length. The note produced at a fret: `note = openNote + fret + capo + transpose`.

Example (length 330 mm): fret 12 → `330 × (1 − 2^(−1)) = 165 mm` (octave at the
middle of the string).

### 3.2a Position relative to the FDC — per-string offset

The theoretical spacing and the calibrated table are both measured **from the nut**
(fret 0 = 0). Each string then carries a single **`fretOffsetMm`** — the distance
from its HOME endstop (the FDC) to the nut — and the axis target is:

```text
absolute position (from FDC) = fretOffsetMm + (calibrated[fret]  or  theory(fret))
```

So `fretOffsetMm` places a whole fretboard relative to its FDC and shifts every
fret of that string at once; it is decoupled from the travel limit `minPositionMm`
(which is a pure clamp). In the web editor the fret table is entered nut-relative,
an **Abs (FDC)** column shows `fretOffsetMm + value`, and **Capture position**
records the live motor position (absolute) minus the offset, so it stores a
nut-relative value that stays correct if the offset is later changed.

### 3.3 Manual calibration (§14.3)

For each fret: (1) select the fret, (2) move the motor with buttons, (3) test the
note, (4) adjust the position, (5) save the exact position. **The calibrated
table takes priority over the theoretical position**: if
`AxisConfig::calibratedFretMm[fret]` is set, `fretPositionMm()` returns the
calibrated value rather than the theoretical one.

### 3.4 Compensation (§14.4)

The system allows: individual correction of a fret, correction according to the
direction of travel (mechanical play / backlash), a global offset for the string,
and forward and backward software limits (`minPositionMm` / `maxPositionMm`,
applied by `clampToLimits`).

---

## 4. Servo calibration (§15)

Each servo uses pulses calibrated in microseconds (`ServoConfig`):

```cpp
enum class ServoSource : uint8_t { Pca = 0, DirectGpio = 1 };

struct ServoConfig {
    bool enabled;
    std::string function;         // "finger"/"pluck"/"strum"/"strumLift"/"damper"/"sharedStrum"/"aux"
    int8_t stringIndex;           // owner string, -1 = shared/global

    ServoSource source;           // PCA9685 OR direct ESP32 GPIO
    uint8_t pcaBoard;             // 0..3 : up to four PCA9685 (0x40..0x43)
    uint8_t channel;              // PCA9685 channel 0..15   (source == Pca)
    int8_t  gpio;                 // ESP32 GPIO            (source == DirectGpio)

    uint16_t pulseMinUs, pulseMaxUs;
    uint16_t restUs, activeUs;    // rest / active position
    bool inverted;
    uint16_t travelMs;            // travel time
    uint16_t settleMs;            // settle time
    bool disableAtRest;           // disable at rest

    // Strum / pluck stroke shaping (per servo).
    uint16_t engageDelayMs;       // strumLift: pause after the lift is down, before the stroke
    bool     alternateDirection;  // alternate down/up stroke on successive strikes
    uint16_t activeAltUs;         // up-stroke active pulse (0 = mirror activeUs about restUs)
    uint16_t strokeMs;            // stroke engage time before return (0 = use travelMs)
    uint16_t minStrikeUs;         // guaranteed minimum strike depth (0 = velocity-only)
};
```

### 4.0a Strum / pluck stroke shaping

For the strike roles (`pluck`, `strum`, `sharedStrum`) MIDI velocity scales the
depth between `restUs` and `activeUs`. Five extra fields control the *gesture*:

* **`alternateDirection` + `activeAltUs`** — successive strokes rake the string
  in opposite directions (down, up, down…). The up-stroke drives to `activeAltUs`,
  or, when that is `0`, to the mirror of `activeUs` about `restUs`. Applies both to
  a per-string striker and to the shared strummer (each servo keeps its own
  stroke parity, reset on neutralise / profile activation).
* **`strokeMs`** — how long the stroke stays engaged before it returns to rest,
  i.e. the stroke's *speed*, independent of `travelMs` (which remains the return /
  settle base). `0` keeps the legacy behaviour (`travelMs`).
* **`minStrikeUs`** — a floor on the strike depth toward the active side so a
  soft (low-velocity) note still catches the string. `0` disables it.
* **`engageDelayMs`** — on a `strumLift` servo, an extra pause after the lift has
  lowered the strum servo onto the string, before the stroke fires. Lets the
  string/lift settle so the attack is clean.

### 4.0b Playback timing / latency (global, `MidiConfig`)

Three global knobs manage the delay between a MIDI Note On and the sound, and how
much the mechanics anticipate to keep that delay small:

* **`noteExecutionDelayMs`** — a **fixed** delay from Note On reception to the note
  actually sounding. The carriage move, finger press and strum prep all happen
  inside this window, so the note plays at a predictable, constant latency
  (`reception + delay`) as long as the mechanics can be ready in time. `0` = play
  as soon as ready (variable latency).
* **`fingerLeadMs`** — begin the finger descent up to this long **before** the
  carriage is estimated to reach the fret, so the finger arrives on the string
  around arrival instead of only starting to descend then. Set too large it can
  drag the finger during the slide, so it is an opt-in value to tune on the bench
  (`0` = press only after arrival, the safe default).
* **`strumLeadMs`** — begin lowering the strum lift up to this long **before** the
  string becomes ready, so the strummer is already engaged when the strike time
  comes. `0` = lower the lift only once the string is ready.

`fingerLeadMs` and `strumLeadMs` shrink the *minimum* achievable
`noteExecutionDelayMs`; all three default to `0` (strictly sequential, safe).

### 4.0 Signal source: PCA9685 or direct GPIO

The system works **with or without a PCA9685**. Each servo independently chooses
its source:

* **PCA9685** — up to **four boards** (`pcaBoard` 0–3, addresses 0x40–0x43),
  i.e. **64 channels** in total; each servo indicates its board and its `channel`
  (0–15). Ideal when the number of servos exceeds the free PWM pins.
* **Direct GPIO** — the servo is driven by a free pin on the ESP32-S3
  (LEDC PWM 50 Hz). Useful without a PCA or for just a few servos.

The two modes can be **mixed** on the same instrument. The validator rejects: a
PCA channel (board + channel) used twice, a direct GPIO that is reserved or in
conflict with a motor signal or another servo, and a per-string role pointing to
a nonexistent string.

### 4.1 Per-string roles

Each string (1 to 6) can have its own servos:

| Role     | Function                                             |
| -------- | ---------------------------------------------------- |
| `finger` | finger press on the fret                             |
| `pluck`  | individual plectrum                                  |
| `strum`  | string-specific strumming                            |
| `damper` | string-specific damper / mute                        |

**Shared** roles (`sharedStrum`, `aux`, `stringIndex = -1`) allow a mechanism
that spans several strings. The firmware lifts the finger and actuates the
string's damper when the note is released.

### 4.1 Finger (§15.1)

Lifted / pressed positions, delay after pressing, delay after releasing. The open
string (fret 0): finger lifted, motor possibly in a safe position, direct plucking
allowed.

### 4.2 Individual plectrum (§15.2)

Left / right / rest positions, automatic alternation, min/max travel, movement
speed or delay.

### 4.3 Open string (§15.3)

Finger lifted, motor possibly moved to a safe position, plucking allowed directly.
Advanced option: use the finger on the zero fret for a specific mechanism.

Recommended layout on a first PCA9685 board (16 channels): 0–5 finger presses,
6–11 individual plucking, 12–15 dampers / shared strumming / auxiliaries. Beyond
that, add boards (`pcaBoard` 1–3) or servos on direct GPIO. Each PCA9685's `/OE`
output is wired to a safety pin — see [`SAFETY.md`](SAFETY.md).
