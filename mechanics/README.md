# Mechanics — reference architecture

Reference mechanical architecture for **Stepper-Plucked-Strings-GMB**
(SPECIFICATION.md §5), and how each mechanical parameter maps to the instrument-profile
fields (`firmware/src/core/motion/StepperAxis.h`, `instrument-profiles/`).

## 1. One independent channel per string (§5.1)

Every string is a self-contained channel. A stepper motor moves a **single**
finger along the string to select the note:

```text
Stepper motor
      ↓
Mechanical transmission
      ↓
Longitudinal carriage
      ↓
Single finger
      ↓
Position on the string
```

Hard invariant (§4, §6): **one movable finger per string, one stepper per
string, never shared**. Active strings = active stepper axes = movable fingers.

Per string the reference build carries (§1):

```text
1 stepper motor        1 finger-press mechanism
1 linear axis          1 pluck mechanism
1 carriage             1 HOME reference sensor
1 single finger
```

## 2. Finger press (§5.2)

Once the carriage is positioned, the finger must be able to press onto the
string, hold it, lift, stay lifted while moving, and stay lifted for an open
string. The reference mechanism is **one servo per string** (PCA9685 channels
0–5). In the profile this is a servo with `function: "finger"`, using `restUs`
(lifted) and `activeUs` (pressed), plus `travelMs`/`settleMs` timing.

Open string: finger stays lifted; the note is plucked directly (§15.3). An
advanced option can instead press "fret 0" for specific mechanics.

## 3. Setting the string vibrating (§5.3)

Two modes, which may be combined on the same instrument:

* **Individual pluck** — one pluck actuator per string (servo `function: "pluck"`,
  PCA9685 channels 6–11). Enables chords, repeated notes, per-string tremolo and
  velocity, and precise per-string triggering.
* **Shared strummer** — one common mechanism sweeping several strings
  (servo `function: "strummer"`/`aux`, channels 12–15). Supports up/down strokes,
  adjustable speed, adjustable string range, string exclusion, return-to-rest and
  synchronisation with the fingers.

The profile field `instrument.pluckMode` selects `individual`, `sharedStrum`, or
`both`.

Per string, up to four servo roles can be defined: `finger` (press), `pluck`
(individual plectrum), `strum` (per-string strum) and `damper`
(per-string mute). Each string also has its own endstop
: the `HOME` reference sensor, plus an optional `LIMIT` switch at the far
end.

## 3.1 Servo signal source: PCA9685 or direct GPIO

Every servo picks its own source, so an instrument can be built **with or without
a PCA9685**, or with a mix of both:

* **PCA9685** — up to **four boards** (`pcaBoard` 0–3, I²C 0x40–0x43 = 64
  channels). Use this once you exceed the ESP32's free PWM pins.
* **Direct GPIO** — the servo hangs off a free ESP32-S3 pin (LEDC 50 Hz PWM),
  handy when there is no PCA or only a couple of servos.

The web interface exposes this choice per servo and prevents channel/pin
conflicts (see [`../docs/CALIBRATION.md`](../docs/CALIBRATION.md) §4).

## 4. Transmission options (§5.1)

The carriage can be driven by any of:

* **GT2 belt** (`transmission: "beltGt2"`)
* **Trapezoidal / ball lead screw** (`transmission: "screw"`)
* **Rack and pinion** — model via `custom`
* **Cable drive** — model via `custom`
* **Experimental** — model via `custom`

## 5. The millimetre abstraction

The firmware never thinks in the transmission's native units. Everything happens
in **millimetres**, converted to motor steps by a transmission-dependent
`stepsPerMm` factor (§5.1, §12.1):

```text
position in millimetres → conversion → position in motor steps
```

This keeps note allocation, motion planning and the profiles independent of the
mechanical type. Theoretical fret positions come from the equal-temperament
formula (§14.2):

```text
position(fret) = scaleLengthMm × (1 − 2^(−fret / 12))
```

A calibrated table (`calibratedFretMm`) overrides theory when present (§14.3).

## 6. Parameter → profile-field mapping

`stepsPerMm` is computed from the transmission (SPECIFICATION.md §12.1):

**Belt (GT2):**

```text
stepsPerMm = (stepsPerRevolution × microsteps) / (pulleyTeeth × beltPitchMm)
```

**Screw:**

```text
stepsPerMm = (stepsPerRevolution × microsteps) / leadPerRevolutionMm
```

**Custom:** use `customStepsPerMm` directly.

| Mechanical quantity | Profile field (`strings[]`) | Used when |
| ------------------- | --------------------------- | --------- |
| Vibrating (scale) length | `scaleLengthMm` | always (fret geometry) |
| Transmission type | `transmission` (`beltGt2`/`screw`/`custom`) | always |
| Motor full steps per revolution | `stepsPerRevolution` (e.g. 200 for 1.8°) | always |
| Driver microstepping | `microsteps` (e.g. 16) | always |
| Pulley tooth count | `pulleyTeeth` | belt |
| Belt tooth pitch | `beltPitchMm` (GT2 = 2 mm) | belt |
| Screw lead per revolution | `leadPerRevolutionMm` (e.g. 8 mm) | screw |
| Explicit steps/mm override | `customStepsPerMm` | custom |
| Direction sense | `invertDirection` | always |
| Soft travel limits | `minPositionMm`, `maxPositionMm` | always |
| Motion profile | `maxSpeedMmS`, `maxAccelMmS2` | always |
| Open-string note | `openNote` (MIDI) | always |
| Highest reachable fret | `maxFret` | always |
| Calibrated fret table | `calibratedFretMm[]` (index = fret) | overrides theory |

### Worked example (GT2 belt)

`stepsPerRevolution = 200`, `microsteps = 16`, `pulleyTeeth = 20`,
`beltPitchMm = 2`:

```text
stepsPerMm = (200 × 16) / (20 × 2) = 3200 / 40 = 80 steps/mm
```

At 16 microsteps and 80 steps/mm the position resolution is 1/80 mm = 12.5 µm,
comfortably finer than fret spacing on every example instrument.

## 7. Homing (§13) — mechanical reference

Each axis references itself against its HOME sensor with a non-blocking state
machine (`CHECK_SENSOR → SEEK_FAST → BACKOFF → SEEK_SLOW → SET_ZERO →
MOVE_TO_OFFSET → READY`). Mechanically relevant profile fields per string
(`strings[].homing`):

| Field | Meaning |
| ----- | ------- |
| `direction` | travel direction toward the sensor (+1 / −1) |
| `fastSpeedMmS` / `slowSpeedMmS` | seek and re-approach speeds |
| `backoffMm` | retreat distance after the first trigger |
| `offsetMm` | final resting offset past zero |
| `timeoutMs` / `maxSearchMm` | fault guards |
| `sensorActiveHigh` | electrical active level (NO/NC support) |

A failing axis is disabled without disturbing the others (§13.2).
