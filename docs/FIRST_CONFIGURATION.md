# First configuration guide — Stepper-Plucked-Strings-GMB

> Source: `SPECIFICATION.md` §8, §10, §26 (first configuration guide).
> Related documents: [`WEB_INTERFACE.md`](WEB_INTERFACE.md) · [`PIN_CONFIGURATION.md`](PIN_CONFIGURATION.md) · [`CALIBRATION.md`](CALIBRATION.md) · [`MIDI_PROTOCOL.md`](MIDI_PROTOCOL.md) · [`SAFETY.md`](SAFETY.md).

This guide walks a beginner from first power-on to the first note, using only
the **simplified mode** of the Web interface. No code modification is needed.

---

## 0. Before you begin

* Power the board and the motors according to the recommended rails (see
  [`SAFETY.md`](SAFETY.md) §6). **Never power the servos from the ESP32
  regulator.**
* At startup, the system is in a safe state: drivers disabled, servos
  neutralized, MIDI queues empty (see [`SAFETY.md`](SAFETY.md) §1). Nothing moves
  until the configuration has been validated.

---

## 1. Connecting to the interface

At first power-on, the ESP32 starts in **access-point mode**:

```text
Default SSID: Stepper-Plucked-Strings-GMB
```

1. Connect your phone/computer to this Wi-Fi network.
2. Open the local address shown (or the captive portal).
3. The configuration wizard opens.

You can later switch to **client mode** (the ESP32 joins your network): SSID,
password, network name, optional static IP, mDNS name. If the connection fails
several times, the system automatically reverts to access-point mode.

---

## 2. Step 1 — Identification

Fill in: instrument name, description (optional), **number of strings** (1 to 6),
instrument type (ukulele, guitar, bass, mandolin, banjo…), proposed tuning,
maximum number of frets. These values determine the note range and are announced
to General-Midi-Boop (see [`MIDI_PROTOCOL.md`](MIDI_PROTOCOL.md) §3).

---

## 3. Step 2 — Board selection

Select the model (**ESP32-S3-DevKitC-1** supported by default), the revision, the
Flash, the presence of PSRAM and the variant. The board profile automatically
sets the GPIO pins that are available, reserved, recommended, and to be used with
caution (see [`PIN_CONFIGURATION.md`](PIN_CONFIGURATION.md)).

> Warning: on certain DevKitC-1 variants, GPIO35/36/37 are used for Flash/PSRAM
> and are not offered without verifying the variant.

---

## 4. Step 3 — Automatic pin assignment

Click **"Assign pins automatically"**. The system chooses a conflict-free
configuration based on the number of strings, the enabled interfaces, the board,
the reservation of the future USB (GPIO19/20), the diagnostics port (UART), the
I²C bus and the sensors. In simplified mode, you only see the **green** pins. If a
signal cannot be placed, the wizard explains it and suggests an alternative.

Example assignment obtained (DevKitC-1 profile, see
[`PIN_CONFIGURATION.md`](PIN_CONFIGURATION.md) §5): STEP on 4/5/6…, DIR on 17/18…,
HOME on 12/13…, I²C SDA 40 / SCL 41, ENABLE 42, PCA9685 safety 47.

---

## 5. Step 4 — Mechanical configuration

For each string: open-string MIDI note, max frets, vibrating length, transmission
type (GT2 belt / screw / custom), motor steps per revolution, microstepping,
travel per revolution, direction inversion, max speed and acceleration, rest
position. The interface **automatically computes the steps/mm** (belt/screw
formulas in [`CALIBRATION.md`](CALIBRATION.md) §1).

---

## 6. Step 5 — Homing

For each axis: sensor enabled, sensor GPIO, NO/NC contact, active level, homing
direction, fast/slow speeds, back-off distance, offset after origin, timeout,
maximum search distance. Homing is non-blocking and independent per string (state
machine `CHECK_SENSOR → … → READY`, see [`CALIBRATION.md`](CALIBRATION.md) §2).

---

## 7. Step 6 — Servo calibration

For each servo: PCA9685 channel, rest position, active position, min/max limits,
inverted direction, travel time, settling time, disable at rest. Typical channel
allocation: fingers 0–5, plucking 6–11, auxiliaries 12–15 (see
[`CALIBRATION.md`](CALIBRATION.md) §4).

---

## 8. Step 7 — Note calibration

Two methods:

* **Automatic fret computation**: `position = longueur vibrante × (1 − 2^(−fret/12))`.
* **Manual calibration**: for each fret, move the motor with the buttons, test
  the note, adjust, and record the exact position. The calibrated table takes
  **priority** over the theory (see [`CALIBRATION.md`](CALIBRATION.md) §3).

---

## 9. Step 8 — Test

Test progressively: each motor, each sensor, each finger, each pick, each note,
each string, a chord, then the **general stop** (STOP). Keep the STOP button
within reach (software panic — see [`SAFETY.md`](SAFETY.md) §3).

---

## 10. Step 9 — Validation

The interface shows **"Configuration valid"** or the precise list of problems.
No actuator is enabled in normal mode as long as critical errors remain
uncorrected. Once valid, the configuration is saved (profile), and the
capabilities are published to General-Midi-Boop.

---

## 11. Connecting General-Midi-Boop (optional)

On the "MIDI > GMB identity and capabilities" page, apply the **General-Midi-Boop**
preset (CC20 = string, CC21 = fret, hybrid mode). GMB then automatically
discovers the instrument (identity, capabilities, strings) via SysEx. See
[`MIDI_PROTOCOL.md`](MIDI_PROTOCOL.md) §2–3 and [`WEB_INTERFACE.md`](WEB_INTERFACE.md) §3.3.

---

## 12. Save and get going

Save your configuration as a profile (at least 8 slots), export it as JSON to
keep it, and set the startup profile. The Wi-Fi password is not included in
ordinary exports.

Enjoy your first note!
