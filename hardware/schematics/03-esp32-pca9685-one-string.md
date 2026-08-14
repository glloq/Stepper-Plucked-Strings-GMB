# Sheet 03 — ESP32 ↔ PCA9685, one string

Signal-level schematic of the **servo side of one string's branch** — the finger
that presses and the plectrum that plucks. The FRET is chosen by the carriage,
not by a servo, so one string needs only a couple of channels here and a single
PCA9685 comfortably serves a whole instrument; the motion side of the same
string is [sheet 04](04-stepper-driver-one-axis.md). Companion to
[`../wiring/WIRING.md`](../wiring/WIRING.md) and
[`../I2C_PCA9685.md`](../I2C_PCA9685.md). Default GPIO are the
ESP32-S3-DevKitC-1 profile's; every pin is reassignable in the web interface
(*Wiring & GPIO*), and the interface draws this exact harness live for your
own configuration.

```text
ESP32-S3-DevKitC-1                              PCA9685 board #n  (string n+1)
──────────────────                              ────────────────────────────────
GPIO40 SDA  ────────●── I²C bus 0 SDA ────────► SDA
GPIO41 SCL  ────────●── I²C bus 0 SCL ────────► SCL
                    │   (pull-ups: ONE equivalent 2.2–4.7 kΩ per bus — see
                    │    I2C_PCA9685.md; most breakouts ship their own)
GPIO47 SERVO_OE ──[enable stage, sheet 02]──●── /OE bus ──► /OE
                                            │
                                    3.3 V ─[R_pu 10k]
GPIO2  ESTOP ◄── NC status loop (sheet 02)
3V3  ──────────────────────────────────────────► VCC   (chip logic)
GND  ──────────────●── common ground ──────────► GND
                                                A0..A2 solder jumpers = n
                                                        (I²C address 0x40 + n)
+V_BR<n> (sheet 01: fuse F<n> + bulk cap C<n>) ► V+    (servo power input)

                     16 servo channels (0…15):
                     ┌───────────────────────────────────────────┐
                     │ ch 0..n-1 : one finger servo per string   │
                     │ ch 6..    : plucker (pluck / strum)       │
                     │ spare     : strumLift / damper / aux      │
                     └───────────────────────────────────────────┘
                     each header pin: PWM ─ V+ ─ GND → servo lead

Direct-GPIO servos (optional, ≤ 8 total): signal from a free ESP32 output pin
(e.g. GPIO4/5/6/7/15–18), power from +V_BR(direct) — never from the ESP32.
```

## Notes

* **Second I²C bus**: bus 1 boards use `SDA2`/`SCL2` (board-profile default —
  GPIO38/39 on a DevKitC-1 **v1.0**, GPIO39/42 on a **v1.1**, whose LED
  occupies GPIO38) and optionally their own `SERVO_OE2` line. Same pattern,
  own pull-ups, own address range 0x40–0x47.
* **Channel map** is the wizard's default convention, not a constraint — any
  servo can live on any `(bus, address, channel)`.
* The `V+` net entering the board is the **fused branch** of sheet 01, with its
  bulk capacitor across `V+`/GND at the board's terminals.
* Full pin capability rules (which GPIO may carry SDA/SCL, `/OE`, `ESTOP`, a
  direct servo) are the board profile's — see
  [`../../board-profiles/`](../../board-profiles/README.md) and
  [`../../docs/PIN_CONFIGURATION.md`](../../docs/PIN_CONFIGURATION.md).
