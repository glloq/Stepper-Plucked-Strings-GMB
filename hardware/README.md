# Hardware — reference electronics

Reference electronics for **Stepper-Plucked-Strings-GMB**, the ESP32-S3 MIDI
machine that drives one stepper-positioned finger per string on plucked- or
strummed-string instruments (1–6 strings). This document describes the reference
architecture of cahier des charges §7; the wiring guide, bill of materials and
Phase 5 CAD deliverables live alongside it.

## Directory

```
hardware/
├── README.md          ← this file (electronics overview, §7)
├── BOM.md             ← bill of materials / nomenclature (§26)
├── wiring/
│   └── WIRING.md      ← connection guide, pinout, power rails (§7 / §22)
├── schematics/
│   └── README.md      ← Phase 5 placeholder (§24)
└── pcb/
    └── README.md      ← Phase 5 placeholder (§24)
```

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
| 12–15 | dampers, shared strummer, or auxiliary functions |

The PCA9685 `/OE` (output-enable) pin must be tied to a **safety GPIO**
(`SERVO_OE`, GPIO47 by default) so all servos can be neutralised instantly on
panic or emergency stop (§21).

### Sensors — HOME / LIMIT (§7.2, §13)

Each axis has a HOME reference sensor (mechanical, optical or Hall). LIMIT
opposite end-stops are optional (0–6). HOME/LIMIT inputs must land on
interrupt-capable GPIO with an appropriate pull (internal or external); the
homing state machine normalises the active level via `sensorActiveHigh`.

## Power (summary, §22)

Four rails, servos on a **separate** supply from the ESP32 regulator:

| Rail | Feeds |
| ---- | ----- |
| 24 V | stepper motors (via the drivers) |
| 5–7.4 V | servomotors |
| 5 V | logic |
| 3.3 V | ESP32-S3 |

Fusing, reverse-polarity protection, a TVS on the motor rail, driver decoupling
and a PCA9685 bulk capacitor are required — see `wiring/WIRING.md` §Power.

## Capacity (§6)

| Resource | Min | Max |
| -------- | :-: | :-: |
| Strings / steppers / fingers / HOME sensors | 1 | 6 |
| Opposite LIMIT switches | 0 | 6 |
| Finger servos | 1 | 6 |
| Pluck servos | 0 | 6 |
| Auxiliary servos | 0 | 4 |
| Total servo outputs | 1 | 16 |

Invariant: **active strings = active stepper axes = movable fingers**.
