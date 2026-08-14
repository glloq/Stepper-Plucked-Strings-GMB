# Stepper-Plucked-Strings-GMB

**Turn a real stringed instrument into a MIDI-controlled robot.**

Send it MIDI notes over Wi-Fi and it plays a ukulele, guitar, bass, mandolin or
banjo for you — a stepper motor slides a "finger" along each string to pick the
note, and small servos press the string and pluck it. Everything is configured
from a **web page in your browser**; no app to install.

> Built for the **ESP32-S3** (and the classic ESP32 boards). The brain is a
> portable, unit-tested C++ core; the ESP32 part is just the hardware glue.

[![CI](https://github.com/glloq/Stepper-Plucked-Strings-GMB/actions/workflows/ci.yml/badge.svg)](https://github.com/glloq/Stepper-Plucked-Strings-GMB/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/ESP32-S3%20%7C%20WROOM--32%20%7C%20DevKit%20v1-informational.svg)](https://www.espressif.com/en/products/socs/esp32)
[![Build](https://img.shields.io/badge/build-PlatformIO%20%7C%20Arduino-orange.svg)](https://platformio.org/)
[![MIDI](https://img.shields.io/badge/MIDI-Wi--Fi%20(UDP)-green.svg)](https://www.midi.org/)

---

## How it works

One **string** = one **motor** + a few **servos**:

```
        ┌──────────────────── one string ────────────────────┐

   nut                      moving finger                   bridge
    │                            ▼                             │
    ╞════════════════════════════●═════════════════════════════╡  ← the string
    0    1    2    3    4    5   fret positions (mm)
    │                            ▲                             │
    │                     stepper motor slides                │
    │                     the finger to the fret              │
    │                                                          │
    └─ servos:  [finger] press down   [pluck] pick the string  ┘
                [damper] mute it       (each string has its own plucker/strummer)
```

To play a note the firmware:

1. **moves** the carriage so the finger sits at the right fret,
2. **presses** the finger with a servo,
3. **plucks** the string,
4. **damps** it when the note ends.

Up to **6 strings** run independently and in parallel, so it can play chords.

### The signal path

```
MIDI over Wi-Fi ─▶ parse ─▶ pick string & fret ─▶ assign notes to strings
                                                        │
                                                        ▼
                                      per-string state machine (move → press → pluck)
                                                        │
                                                        ▼
                                        stepper motors  +  servos (PCA9685 or GPIO)
```

A controller such as **General-MIDI-Boop** can also ask the instrument, over MIDI
SysEx, *"how many strings do you have, what's your range, which CCs do you
understand?"* and adapt automatically.

---

## What it looks like

Everything is configured and played from a web page served by the ESP32 — three
pages and a settings modal, no app to install.

![The Instrument page](img/screenshots/instrument.png)

*The **Instrument** page: one lane per string with each carriage drawn where it
actually is. Click a fret to play it — that sends a real MIDI note through the
whole chain, so it tests what a controller would get.*

| | |
| --- | --- |
| [![Setup wizard](img/screenshots/setup-mechanics.png)](docs/WEB_INTERFACE.md#3-setup--nine-steps) | [![Wiring harness](img/screenshots/wiring.png)](docs/WEB_INTERFACE.md#41-harness) |
| **Setup** — nine steps from identity to validation | **Wiring & GPIO** — the harness of *your* configuration |
| [![Power and safety](img/screenshots/wiring-power.png)](docs/WEB_INTERFACE.md#42-power--safety) | [![Diagnostics](img/screenshots/settings-diagnostics.png)](docs/WEB_INTERFACE.md#52-diagnostics) |
| **Power & safety** — the reference circuit, sized for your currents | **Diagnostics** — loop jitter, dropped MIDI, motion counters |

The full tour, with every page, is in
[`docs/WEB_INTERFACE.md`](docs/WEB_INTERFACE.md). All the screenshots are
generated from the real interface running its mock backend — no device needed:
`node web-interface/tools/screenshots.js`.

---

## Features

- 🎸 **1–6 strings**, each with its own motor, finger, plucker and optional damper.
- 🎵 **Automatic note allocation** — send plain MIDI notes and it spreads chords
  across the strings, or **force an exact string/fret** with MIDI CC (tablature).
- 🤙 **Per-string plucking** — every string has its own striker: a plectrum
  plucker or a per-string strum servo.
- 🛰️ **Wi-Fi MIDI** — plays notes received over the network (UDP, port 5006).
- 🖥️ **Local web interface** — setup wizard, live dashboard, MIDI monitor, SysEx
  tester. Runs entirely on the ESP32, no cloud.
- 🧩 **Capability announcement (SysEx)** so a host discovers the instrument.
- 🛡️ **Safety first** — homing before any play, a **boot-safe** start (with no
  valid profile it locks into CONFIG_SAFE instead of inventing one), a real
  hard-stop path distinct from the controlled park, per-axis fault isolation,
  endstop monitoring and fail-safe E-stop wiring.
- ⚡ **In-rush governor** — chords stagger the current-hungry starts (carriage
  repositioning, finger presses) while the strikes that carry the sound are
  never throttled.
- 📈 **Runtime diagnostics** — `GET /api/diagnostics`: loop latency and jitter,
  dropped MIDI, homing failures, LIMIT trips, move timeouts, per-board PCA
  health.
- 🔧 **Servo driving your way** — PCA9685 boards over **either** of the ESP32-S3's
  two I²C buses *or* direct ESP32 pins, mixable per servo.
- 🧰 **Three boards supported** — ESP32-S3-DevKitC-1 (both revisions),
  ESP32-WROOM-32 and ESP32 DevKit v1; each one is compiled in CI, which is what
  "supported" means here.

### Instruments it already knows

Ready-made profiles live in [`instrument-profiles/`](instrument-profiles/):
**ukulele**, **guitar**, **bass**, **mandolin**, **banjo**. Each is a JSON file
you can tweak or copy from the web wizard.

---

## Quick start

### 1. Try the logic on your PC (no hardware)

The whole musical brain is plain C++ and runs on your laptop:

```bash
cd firmware/test
make            # builds and runs the unit-test suite
```

You should see `… tests, … checks, 0 failures` (the CI badge is the source of
truth for the count).

### 2. Build and flash the firmware

You can use **PlatformIO** or the **Arduino IDE** — same source.

**PlatformIO**

```bash
cd firmware
./sync_web_data.sh          # copy the web UI into the LittleFS image
pio run                     # build for the ESP32-S3-DevKitC-1 (default)
pio run -e esp32-wroom-32   # …or a classic ESP32 board
pio run -t uploadfs         # upload the web interface
pio run -t upload           # flash the firmware
```

**Arduino IDE** — open `firmware/firmware.ino` (the `src/` folder is compiled
recursively). Full guide: [`docs/ARDUINO_IDE.md`](docs/ARDUINO_IDE.md).

### 3. First configuration

On first boot the ESP32 creates a Wi-Fi access point called
**`Stepper-Plucked-Strings-GMB`**. Connect to it, open the device's address in a
browser, and the **setup wizard** walks you through pins, strings and servos.
See [`docs/FIRST_CONFIGURATION.md`](docs/FIRST_CONFIGURATION.md).

---

## Repository layout

```text
Stepper-Plucked-Strings-GMB/
├── firmware/            ESP32-S3 firmware
│   ├── src/core/        Portable C++ logic (MIDI, allocation, motion, safety) — unit-tested
│   ├── src/platform/    ESP32 adapters (Wi-Fi, web server, drivers, storage)
│   ├── src/main.cpp     Hardware integration / entry point
│   └── test/            Native test suite (runs with g++)
├── web-interface/       Local web app (wizard, dashboard, MIDI monitor, SysEx tester)
├── instrument-profiles/ Example instruments (ukulele, guitar, bass, mandolin, banjo)
├── board-profiles/      Board pin maps — GENERATED from BoardProfile.cpp
├── hardware/            Reference electronics, schematics, commissioning, BOM
├── mechanics/           Per-string mechanical design
└── docs/                Guides and reference (see below)
```

**Software design in one line:** a pure C++17 core (`firmware/src/core/`, no
Arduino dependency, tested on a PC) plus thin ESP32 adapters
(`firmware/src/platform/esp32/`). Details in
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

---

## Documentation

| Guide | What's inside |
| ----- | ------------- |
| [Architecture](docs/ARCHITECTURE.md) | How the code is structured |
| [First configuration](docs/FIRST_CONFIGURATION.md) | Setup wizard walkthrough |
| [Web interface](docs/WEB_INTERFACE.md) | Every page of the local UI |
| [MIDI protocol](docs/MIDI_PROTOCOL.md) | Notes, CC string/fret selection, SysEx |
| [Pin configuration](docs/PIN_CONFIGURATION.md) | GPIO assignment & validation |
| [Calibration](docs/CALIBRATION.md) | Fret positions, homing, mechanics |
| [Safety](docs/SAFETY.md) | Boot-safe, homing, hard-stop vs park, E-stop, faults |
| [Power & safety circuit](hardware/POWER_AND_SAFETY.md) | The reference electrical architecture: three rails, E-stop chain, `/OE` and driver `ENABLE` |
| [Commissioning](hardware/COMMISSIONING.md) | Staged power-up acceptance procedure |
| [Schematics](hardware/schematics/README.md) | Power distribution, E-stop, PCA branch, stepper driver |
| [Network & hotspot](docs/NETWORK_HOTSPOT.md) | Station/AP, the BOOT-button hotspot, captive portal |
| [Device vs instrument](docs/DEVICE_INSTRUMENT.md) | What travels with the machine and what travels with the instrument |
| [Generalisation](docs/GENERALIZATION.md) | Where the 6-string / 24-fret assumptions live |
| [Arduino IDE](docs/ARDUINO_IDE.md) | Building without PlatformIO |

The original specifications are the three markdown files at the repository root:
the full requirements ([`SPECIFICATION.md`](SPECIFICATION.md)), the string/fret
CC selection spec ([`STRING_FRET_SELECTION.md`](STRING_FRET_SELECTION.md)), and
the SysEx capability protocol ([`SYSEX_CAPABILITIES.md`](SYSEX_CAPABILITIES.md)).

---

## Project status

**What is done and verified in CI:**

- Complete, unit-tested logic core (native tests under `-Werror`, and again
  under AddressSanitizer + UBSan).
- Three ESP32 board builds (PlatformIO) plus a fast host compile-check of
  `main.cpp` and every platform adapter.
- Runtime harnesses for the Arduino-gated code that unit tests cannot reach:
  `servobankcheck` (dual-bus routing, controlled park, `ActuatorResult`) and
  `stepperbankcheck` (hard-stop vs controlled stop, refused moves, soft limits,
  a missing step generator).
- Every shipped instrument profile loaded through the real firmware parser,
  plus the v1→v2 migration and the split-slot round trip.
- The JSON board profiles are checked to still match `BoardProfile.cpp` — the
  wizard's pin table and the validator's pin table cannot drift apart.
- Web interface (vanilla JS, no build step) syntax-checked and behaviourally
  tested.

**Not yet done — hardware validation.** The firmware has **not** been run against
a physical instrument. STEP timing on a logic analyzer, six simultaneous axes,
MIDI endurance, faulty/missing/inverted sensor behaviour, the real in-rush of a
chord even with the governor, and whether a hard stop cuts fast enough under
load all still need a real test bench. Everything above is verified *in
software*. Start with [`hardware/COMMISSIONING.md`](hardware/COMMISSIONING.md). Treat the current state as **ready for bench bring-up**, not for an
unattended, fully-strung instrument under power.

Known limitations and roadmap are listed at the bottom of
[`docs/SAFETY.md`](docs/SAFETY.md) and throughout the docs.

### Safety note

The software emergency-stop is a convenience, **not** a substitute for a hardware
cut of the driver `ENABLE` and of motor power. A stepper holds its position by
burning current in its coils: stopping the STEP pulses stops *motion*, but the
driver stays energised, the motor stays hot and the carriage stays clamped until
`ENABLE` goes inactive or the rail disappears. Wire a physical E-stop before
putting motors under load — reference circuit in
[`hardware/POWER_AND_SAFETY.md`](hardware/POWER_AND_SAFETY.md), firmware
behaviour in [`docs/SAFETY.md`](docs/SAFETY.md).
