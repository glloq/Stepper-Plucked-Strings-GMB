# Bill of materials (nomenclature)

Reference bill of materials for **Stepper-Plucked-Strings-GMB** (cahier des
charges §26). Quantities scale with the string count *N* (1–6). This is the
prototype/reference build with **pluggable driver modules** (§7.2); the
integrated PCB variant is a Phase 5 deliverable (see `hardware/pcb/`).

Part numbers are indicative references, not a mandated sourcing list.

## Electronics — core

| Ref | Qty | Item | Notes |
| --- | :-: | ---- | ----- |
| U1 | 1 | ESP32-S3-DevKitC-1 | main controller (§7.1); verify Flash/PSRAM variant vs GPIO33–37 |
| U2 | 1 | PCA9685 16-ch PWM/servo driver breakout | I²C servo expander (§7.3) |
| U3 | *N* | TMC2209 stepper driver module | pluggable STEP/DIR driver, one per string (§7.2) |
| — | 1 | Driver carrier / socket header set | pluggable module sockets |

## Actuators & motors

| Ref | Qty | Item | Notes |
| --- | :-: | ---- | ----- |
| M1..M*N* | *N* | Stepper motor (NEMA, 1.8°, 200 steps/rev) | one per string; size to axis load |
| SV_F | *N* | Servo — finger press | PCA9685 channels 0..N−1 |
| SV_P | 0–*N* | Servo — individual pluck | PCA9685 channels 6..6+N−1 |
| SV_A | 0–4 | Servo — damper / shared strummer / aux | PCA9685 channels 12–15 |

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
| — | 1 | Pluck mechanism (or shared strummer for the machine) | individual pick or shared |
| — | 1 | HOME sensor mount + flag | reference datum |

## Power

| Ref | Qty | Item | Notes |
| --- | :-: | ---- | ----- |
| PS1 | 1 | 24 V PSU | stepper motor rail (§22) |
| PS2 | 1 | 5–7.4 V PSU / BEC | **separate** servo rail — never the ESP regulator |
| PS3 | 1 | 5 V buck converter | logic rail |
| — | 1 | 3.3 V regulator | on the ESP32-S3 board |

## Protection & passives

| Ref | Qty | Item | Notes |
| --- | :-: | ---- | ----- |
| F1 | 1 | Fuse — motor rail | rate to motor current (§22) |
| F2 | 1 | Fuse — servo rail | rate to combined servo current |
| D1 | 1 | Reverse-polarity protection (diode / P-MOS) | on incoming supply |
| TVS1 | 1 | TVS diode — 24 V motor rail | transient clamp |
| C_drv | 2×*N* | Decoupling caps near each driver | electrolytic + ceramic |
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
| — | 1 | E-stop switch | forces drivers off + PCA9685 `/OE` high (§21.2) |
| — | as needed | Wire, ferrules, GT2 belt/pulley or lead screw, fasteners | mechanics |

## Notes

* **Servo count** = finger (*N*, mandatory ≥1) + pluck (0–*N*) + aux (0–4),
  total ≤ 16 across the PCA9685 (§6).
* **Driver current** must be set per TMC2209 to the motor rating before use.
* Confirm the ESP32-S3 module variant: octal-PSRAM parts consume GPIO35–37 (kept
  reserved in the board profile).
