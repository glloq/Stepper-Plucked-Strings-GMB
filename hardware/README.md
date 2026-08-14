# Hardware — reference electronics

Reference electronics for **Stepper-Plucked-Strings-GMB**, the ESP32-S3 MIDI
machine that drives one stepper-positioned finger per string on plucked- or
strummed-string instruments (1–6 strings). This document describes the reference
architecture of SPECIFICATION.md §7; the wiring guide, bill of materials and
Phase 5 CAD deliverables live alongside it.

## Directory

```
hardware/
├── README.md              ← this file (electronics overview, §7)
├── POWER_AND_SAFETY.md    ← the REFERENCE circuit: three rails, E-stop chain,
│                            fail-safe /OE and driver ENABLE, sizing method
├── COMMISSIONING.md       ← staged power-up acceptance procedure
├── I2C_PCA9685.md         ← bus topology, addressing, pull-ups
├── BOM.md                 ← bill of materials / nomenclature (§26)
├── wiring/
│   └── WIRING.md          ← connection guide, pinout, power rails (§7 / §22)
├── schematics/
│   ├── 01-power-distribution.md
│   ├── 02-estop-and-servo-enable.md
│   ├── 03-esp32-pca9685-one-string.md   (servo side of a string)
│   └── 04-stepper-driver-one-axis.md    (motion side of a string)
└── pcb/
    └── README.md          ← Phase 5 placeholder (§24)
```

**Start with [`POWER_AND_SAFETY.md`](POWER_AND_SAFETY.md)** — it is the one
circuit the firmware, the BOM and the web interface all describe. Then bring the
machine up with [`COMMISSIONING.md`](COMMISSIONING.md), which is also mirrored as
a live checklist in the web interface (*Wiring & GPIO → Commissioning*).

## Block diagram (§7)

```text
                         Wi-Fi
                           │
               MIDI + web configuration
                           │
                           ▼
                       ESP32-S3
                           │
        ┌──────────────────┼──────────────────┐
        │                  │                  │
   STEP / DIR / EN        I²C              Sensors
        │                  │                  │
   1–6 TMC2209          PCA9685          HOME / LIMIT
        │                  │
   1–6 steppers      1–16 servos
```

## Major blocks

### Main controller — ESP32-S3 (§7.1)

The reference controller is an **ESP32-S3-DevKitC-1**. It handles Wi-Fi MIDI
transport, hosts the web configurator, allocates notes, plans motion, runs the
per-string state machines, drives the PCA9685, monitors the sensors, stores
profiles, and enforces safety. Its GPIO matrix lets peripheral signals be routed
to many pins, which is what makes the configurable board profiles and pin
assignment possible (`board-profiles/esp32-s3-devkitc-1.json`).

### Stepper drivers — 1–6 × TMC2209 (§7.2)

One STEP/DIR-compatible driver per string; the reference is the **TMC2209**.
Each axis exposes STEP, DIR, ENABLE and HOME, with optional LIMIT, DIAG and UART.
The first prototype board must accept **pluggable driver modules** so a driver
can be swapped, different models tried, motor current tuned, and maintenance
done before an integrated PCB exists.

Per-axis signals:

```text
STEP        (fast output from ESP32-S3)
DIR         (output)
ENABLE      (shared global ENABLE line, GPIO42 by default)
HOME        (reference sensor input, interrupt-capable)
LIMIT       (optional opposite end-stop)
DIAG        (optional TMC2209 stall/diag)
UART        (optional TMC2209 configuration)
```

### Servo expander — PCA9685 (§7.3)

A single **PCA9685** provides up to 16 servo channels over I²C. Recommended
channel map:

| Channels | Use |
| -------- | --- |
| 0–5 | finger press (one per string) |
| 6–11 | individual pluck (one per string) |
| 12–15 | dampers or auxiliary functions |

The PCA9685 `/OE` (output-enable) pin must be tied to a **safety GPIO**
(`SERVO_OE`, GPIO47 by default) so all servos can be neutralised instantly on
panic or emergency stop (§21).

### Sensors — HOME / LIMIT (§7.2, §13)

Each axis has a HOME reference sensor (mechanical, optical or Hall). LIMIT
opposite end-stops are optional (0–6). HOME/LIMIT inputs must land on
interrupt-capable GPIO with an appropriate pull (internal or external); the
homing state machine normalises the active level via `sensorActiveHigh`.

## Power (summary, §22)

Four rails, servos and motors each on a **separate** supply from the ESP32
regulator:

| Rail | Feeds | Dropped by the E-stop? |
| ---- | ----- | ---------------------- |
| 12–24 V | stepper motors (via the drivers' `VMOT`) | **yes — K2** |
| 5–7.4 V | servomotors (PCA9685 `V+` and direct-GPIO servos) | **yes — K1** |
| 5 V | logic | no |
| 3.3 V | ESP32, PCA9685 `VCC`, driver logic, the `/OE` and `ENABLE` pull-ups | no |

Fusing, reverse-polarity protection, a TVS on the motor rail, driver decoupling
and a PCA9685 bulk capacitor are required. **Both power rails must go away on an
E-stop**, and so must the driver `ENABLE`: a stepper holds its position by
burning current in the coils, so cutting the STEP pulses stops the *motion* while
the driver stays energised, the motor stays hot and the carriage stays clamped.
Full circuit and sizing method: [`POWER_AND_SAFETY.md`](POWER_AND_SAFETY.md).

## Capacity (§6)

| Resource | Min | Max |
| -------- | :-: | :-: |
| Strings / steppers / fingers / HOME sensors | 1 | 6 |
| Opposite LIMIT switches | 0 | 6 |
| Finger servos | 1 | 6 |
| Pluck servos | 0 | 6 |
| Auxiliary servos | 0 | 4 |
| Total servo outputs | 1 | 16 per PCA9685 board |
| PCA9685 boards | 0 | 8 per I²C bus × 2 buses = 16 |
| Direct-GPIO servos | 0 | 8 (one LEDC channel each) |

Invariant: **active strings = active stepper axes = movable fingers**.
