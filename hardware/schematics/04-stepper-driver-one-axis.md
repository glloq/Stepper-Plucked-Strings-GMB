# Sheet 04 — STEP/DIR driver, one axis

Signal-level schematic of the **motion side of one string**: the carriage that
chooses the fret. The pattern repeats per axis (1..6); the servo side of the
same string is [sheet 03](03-esp32-pca9685-one-string.md). Default GPIO are the
ESP32-S3-DevKitC-1 profile's; every pin is reassignable in the web interface
(*Wiring & GPIO*), which draws the per-axis table live for your configuration.

```text
ESP32-S3-DevKitC-1                          STEP/DIR driver A<n>  (axis n)
──────────────────                          ──────────────────────────────────
GPIO4  STEP1  ─────────────────────────────► STEP    (one pulse = one microstep)
GPIO17 DIR1   ─────────────────────────────► DIR     (level = travel direction)
GPIO42 ENABLE ──[enable stage, sheet 02]──●─► /EN    (ACTIVE-LOW, shared by ALL
                                          │          drivers — all or nothing)
                                  3.3 V ─[R_en 10k]  ◄── mandatory: holds the
                                                         drivers RELEASED while
                                                         the ESP32 is absent,
                                                         resetting or unplugged
3V3  ─────────────────────────────────────► VDD / VIO  (driver logic supply)
GND  ────────────────●── common ground ───► GND

+V_MOTOR (sheet 01: fuse F<n>m + bulk cap C<n>m ≥100 µF at the driver)
                     ─────────────────────► VMOT   (12–24 V motor supply)
                                            1A 1B 2A 2B ──► motor phases

MICROSTEPPING            MS1 MS2 MS3 jumpers (A4988 / DRV8825) or UART (TMC2209)
                         must match the profile's `microsteps` — a mismatch
                         scales every fret position by the same wrong factor.

Vref                     set to the motor's RATED PHASE CURRENT before the first
                         motion, and MEASURE it (test point / pot wiper). Too
                         high cooks the motor; too low loses steps, and a lost
                         step is a note at the wrong pitch nothing can detect.

ENDSTOPS
GPIO12 HOME1  ◄──────────── HOME switch   (the reference: seek target at boot)
GPIO13 LIMIT1 ◄──────────── LIMIT switch  (opposite end: catches a missed HOME)
                            both INPUT_PULLUP, debounced in firmware, wired as
                            NC (closed = not triggered) so a cut wire reads
                            TRIGGERED and faults the axis instead of hiding it
```

## Notes

* **HOME and LIMIT sit at OPPOSITE ends of the travel.** HOME is what the axis
  seeks at boot to anchor 0 mm; LIMIT is the backstop that says the carriage ran
  past where HOME should have been. During homing the firmware watches LIMIT in
  every phase and hard-stops + faults the axis the moment it trips, instead of
  grinding on until the search-distance timeout.
* **Polarity is per axis**, in the profile (`homing.sensorActiveHigh`,
  `homing.limitActiveHigh`) — mechanical switches, optical and inductive sensors
  do not agree on a resting level. Verify each one by hand at commissioning
  stage 1 before any motion.
* **DIR inverted** is the classic first-run fault: the axis runs *away* from
  HOME on the seek and stops at the far end. Fix it with `invertDirection` in
  the profile (or by swapping one motor phase pair), not by moving the sensor.
* **A released driver loses the position reference.** Anything that drops
  `ENABLE` — an E-stop, a panic, a profile change — means the carriage may have
  moved, so recovery always requires a full re-home. There is no "resume where
  we were", by design.
* On a **vertical or spring-loaded** axis, releasing `ENABLE` lets the carriage
  fall: add a mechanical brake or counterweight. Holding torque is not a safety
  function — removing it is precisely what the E-stop does.
* **STEP needs a fast output.** The board profile marks which GPIO can carry it
  (`highSpeedOutput`); the validator refuses a STEP line on a pin that cannot.
  On a classic ESP32 the RMT/MCPWM units are fewer than on the S3, so a 6-axis
  instrument is realistically an S3 job — an axis that cannot attach a hardware
  step generator is faulted at boot rather than silently not stepping.
* Full pin capability rules are the board profile's — see
  [`../../board-profiles/`](../../board-profiles/README.md) and
  [`../../docs/PIN_CONFIGURATION.md`](../../docs/PIN_CONFIGURATION.md).
