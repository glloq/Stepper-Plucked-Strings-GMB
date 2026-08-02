# Instrument profiles

Example configuration profiles for **Stepper-Plucked-Strings-GMB**. Each file is
a full, valid instrument profile matching the JSON format of cahier des charges
§20 and the firmware `gmb::Profile` (`firmware/src/core/configuration/Profile.h`).

They are meant as realistic starting points: import one from the web interface,
then calibrate homing, fret positions and servos for your physical build.

## Files

| File | Instrument | Strings | Tuning (MIDI) | Frets | Scale | Pluck mode |
| ---- | ---------- | :-----: | ------------- | :---: | :---: | ---------- |
| `ukulele-gcea.json` | Soprano ukulele | 4 | G4 C4 E4 A4 — 67 60 64 69 | 12 | 330 mm | individual |
| `guitar-standard.json` | Guitar, standard | 6 | E2 A2 D3 G3 B3 E4 — 40 45 50 55 59 64 | 19 | 648 mm | both |
| `bass-4string.json` | Bass guitar | 4 | E1 A1 D2 G2 — 28 33 38 43 | 20 | 864 mm | individual |
| `mandolin-gdae.json` | Mandolin (4 courses) | 4 | G3 D4 A4 E5 — 55 62 69 76 | 12 | 350 mm | sharedStrum |
| `banjo-5string.json` | Banjo, open-G | 5 | D3 G3 B3 D4 + g G4 — 50 55 59 62 67 | 22 (5th: 17) | 660 mm | individual |

MIDI note reference: 60 = C4 (middle C).

## What varies between examples

The set is intentionally diverse so it exercises the schema:

* **Pluck modes** — `individual` (ukulele, bass, banjo), `sharedStrum`
  (mandolin), and `both` (guitar, which also carries a shared strummer on an aux
  channel). See cahier des charges §5.3.
* **Transmissions** — `beltGt2` (ukulele, guitar, banjo), `screw` (bass,
  long-scale lead screw), and `custom` (mandolin, `customStepsPerMm`). See §12.1.
* **String/fret selection** — the guitar sets `stringFretSelection.string.reverseOrder = true`
  (string CC counts from the high E), and the mandolin uses an explicit
  `mapping` `[3, 2, 1, 0]` to reverse logical → physical order. All others use
  the identity mapping.
* **Re-entrant strings** — the ukulele high-G and the banjo short 5th string
  (its travel is limited to 17 frets while the rest reach 22).

## Conventions shared by every profile

* **Pins** follow the recommended ESP32-S3-DevKitC-1 table (§11.5): `STEP` on
  4/5/6/7/15/16, `DIR` on 17/18/8/9/10/11, `HOME` on 12/13/14/21/38/39, I²C
  `SDA=40` / `SCL=41`, global `ENABLE=42`, PCA9685 `SERVO_OE=47`. Only the first
  *N* rows are used for an *N*-string instrument. `board.automaticPinAssignment`
  is `false` because the pins are written out explicitly.
* **Servos on the PCA9685** — finger servos on channels `0 … N−1`, individual
  pluck servos on channels `6 … 6+N−1`, and any shared strummer on channel 12
  (the aux band 12–15 of §7.3).
* **One `strings[]` entry per string**, each with its own `homing` block.
* **Selection ranges track the instrument** — `stringFretSelection.string.maximum`
  equals the string count and `.fret.maximum` equals the largest `maxFret`.
* `calibratedFretMm` is left empty (`[]`); positions are computed from the
  theoretical fret formula (§14.2) until you run manual calibration (§14.3),
  after which the calibrated table takes priority.

## Editing / validating

These are plain JSON. After editing, confirm the file still parses:

```sh
python3 -m json.tool instrument-profiles/guitar-standard.json > /dev/null
```

The firmware `ProfileValidator` performs the full semantic check (pin conflicts,
servo channel ranges, selection bounds) when a profile is imported.
