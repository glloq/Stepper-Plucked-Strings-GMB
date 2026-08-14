# Bill of materials

Reference bill of materials for **Stepper-Plucked-Strings-GMB**
(SPECIFICATION.md §26). Quantities scale with the string count *N* (1–6). This is the
prototype/reference build with **pluggable driver modules** (§7.2); the
integrated PCB variant is a Phase 5 deliverable (see `hardware/pcb/`).

Part numbers are indicative references, not a mandated sourcing list.

## Electronics — core

| Ref | Qty | Item | Notes |
| --- | :-: | ---- | ----- |
| U1 | 1 | ESP32-S3-DevKitC-1 | main controller (§7.1); verify Flash/PSRAM variant vs GPIO33–37 |
| U2 | 1–16 | PCA9685 16-ch PWM/servo driver breakout | I²C servo expander (§7.3); addresses 0x40–0x47 set by the A0–A2 jumpers, on either of the two I²C buses |
| U3 | *N* | TMC2209 stepper driver module | pluggable STEP/DIR driver, one per string (§7.2) |
| — | 1 | Driver carrier / socket header set | pluggable module sockets |

## Actuators & motors

| Ref | Qty | Item | Notes |
| --- | :-: | ---- | ----- |
| M1..M*N* | *N* | Stepper motor (NEMA, 1.8°, 200 steps/rev) | one per string; size to axis load |
| SV_F | *N* | Servo — finger press | PCA9685 channels 0..N−1 |
| SV_P | 0–*N* | Servo — individual pluck | PCA9685 channels 6..6+N−1 |
| SV_A | 0–4 | Servo — damper / aux | PCA9685 channels 12–15 |

## Sensors

| Ref | Qty | Item | Notes |
| --- | :-: | ---- | ----- |
| S_H | *N* | HOME reference sensor (optical / Hall / microswitch) | one per axis (§13) |
| S_L | 0–*N* | LIMIT opposite end-stop | optional |

## Mechanics — per string (see `mechanics/README.md`)

| Ref | Qty (per string) | Item | Notes |
| --- | :-: | ---- | ----- |
| — | 1 | Linear guide (rail + carriage) | longitudinal finger travel |
| — | 1 | Transmission set | GT2 belt + 20T pulley + idler, **or** lead screw + nut, **or** rack, **or** cable |
| — | 1 | Carriage / finger assembly | single movable finger |
| — | 1 | Finger-press mechanism | servo-actuated |
| — | 1 per string | Pluck/strum mechanism per string | individual pick per string |
| — | 1 | HOME sensor mount + flag | reference datum |

## Power

| Ref | Qty | Item | Notes |
| --- | :-: | ---- | ----- |
| PS1 | 1 | 12–24 V PSU | stepper motor rail (§22); size the CONTINUOUS rating for the holding floor (n_axes × Vref) — an armed stepper never stops drawing |
| PS2 | 1 | 5–7.4 V PSU / BEC | **separate** servo rail — never the ESP regulator |
| PS3 | 1 | 5 V buck converter | logic rail |
| — | 1 | 3.3 V regulator | on the ESP32-S3 board |

## Protection & passives

| Ref | Qty | Item | Notes |
| --- | :-: | ---- | ----- |
| F0m | 1 | Fuse — motor rail (main) | every axis at its moving current |
| F1m..Fnm | *N* | Fuse — per driver branch | one axis's moving current + headroom |
| F0 | 1 | Fuse — servo rail (main) | whole-instrument worst case |
| F1..Fn | per branch | Fuse — per PCA / direct-servo branch | see POWER_AND_SAFETY.md §1.2 |
| D1 | 1 | Reverse-polarity protection (diode / P-MOS) | on incoming supply |
| TVS1 | 1 | TVS diode — 24 V motor rail | transient clamp |
| C_drv | 2×*N* | Decoupling caps AT each driver's `VMOT` | ≥100 µF electrolytic rated well above the rail (35–50 V on a 24 V rail) + 100 nF ceramic. Never hot-plug a driver: the inrush into an uncharged cap kills them |
| C_pca | 1 | Bulk reservoir cap ≥ 470 µF near PCA9685 `V+` | servo inrush |
| R_i2c | 2 | I²C pull-ups (2.2–4.7 kΩ to 3.3 V) | if not on the PCA9685 breakout |
| R_sns | 0–*N* | Sensor pull resistors | if internal pulls not used |

## Interconnect

| Ref | Qty | Item | Notes |
| --- | :-: | ---- | ----- |
| J_mot | *N* | Lockable motor connector (4-pin) | per axis |
| J_sv | up to 16 | Servo 3-pin headers | on/from the PCA9685 |
| J_sns | *N* (+LIMIT) | Sensor connector (3-pin) | HOME / LIMIT |
| J_pwr | 3 | Lockable power connectors | 24 V / servo / 5 V |
| — | 1 | E-stop button — **latching, ≥2 NC contacts** | mushroom head, twist/pull release. NC #1 drops both contactors, NC #2 closes the `ESTOP` status loop, NC #3 gates the `/OE` and `ENABLE` stages (§21.2, POWER_AND_SAFETY.md §2) |
| K1 | 1 | Contactor / relay — servo rail | **DC-rated** at the rail current (a DC arc is far harder to break than AC) |
| K2 | 1 | Contactor / relay — motor rail | DC-rated; both coils in series on NC #1 |
| S1 | 1 | Master switch | upstream of K1/K2 on a fixed installation |
| R_oe | 1–2 | `/OE` pull-up, 10 kΩ to 3.3 V | **mandatory** — a floating `/OE` means outputs ENABLED |
| R_en | 1 | `ENABLE` pull-up, 10 kΩ to 3.3 V | **mandatory** — a floating `/EN` reads LOW on most drivers, so the coils energise with the ESP32 absent |
| — | as needed | Wire, ferrules, GT2 belt/pulley or lead screw, fasteners | mechanics |

## Notes

* **Servo count** = finger (*N*, mandatory ≥1) + pluck (0–*N*) + aux (0–4). One
  PCA9685 (16 channels) covers a typical instrument; more boards are available on
  both I²C buses if needed (§6).
* **Driver current (Vref)** must be set to the motor's rated phase current before
  the first motion, and **measured** — not trusted from the pot position. Too
  high cooks the motor and driver; too low loses steps, and a lost step is a note
  at the wrong pitch that no software can detect.
* **Heatsink the drivers.** The holding current runs for the entire time the
  instrument is armed, not just during moves.
* Confirm the ESP32-S3 module variant: octal-PSRAM parts consume GPIO35–37 (kept
  reserved in the board profile).
