# Stepper-Plucked-Strings-GMB

**Turn a real stringed instrument into a MIDI-controlled robot.**

Send it MIDI notes over Wi-Fi and it plays a ukulele, guitar, bass, mandolin or
banjo for you — a stepper motor slides a "finger" along each string to pick the
note, and small servos press the string and pluck it. Everything is configured
from a **web page in your browser**; no app to install.

> Built for the **ESP32-S3**. The brain is a portable, unit-tested C++ core; the
> ESP32 part is just the hardware glue.

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
                [damper] mute it       (+ optional shared strummer for chords)
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

## Features

- 🎸 **1–6 strings**, each with its own motor, finger, plucker and optional damper.
- 🎵 **Automatic note allocation** — send plain MIDI notes and it spreads chords
  across the strings, or **force an exact string/fret** with MIDI CC (tablature).
- 🤙 **Three plucking styles** — one plucker per string, a single shared strummer
  for chords, or both.
- 🛰️ **Wi-Fi MIDI** — plays notes received over the network (UDP, port 5006).
- 🖥️ **Local web interface** — setup wizard, live dashboard, MIDI monitor, SysEx
  tester. Runs entirely on the ESP32, no cloud.
- 🧩 **Capability announcement (SysEx)** so a host discovers the instrument.
- 🛡️ **Safety first** — homing before any play, emergency-stop handling, endstop
  monitoring, and a fail-safe boot.
- 🔧 **Servo driving your way** — a PCA9685 board over I²C *or* direct ESP32 pins.

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

You should see `129 tests, … checks, 0 failures`.

### 2. Build and flash the firmware

You can use **PlatformIO** or the **Arduino IDE** — same source.

**PlatformIO**

```bash
cd firmware
./sync_web_data.sh          # copy the web UI into the LittleFS image
pio run                     # build for the ESP32-S3-DevKitC-1
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
├── board-profiles/      Board pin maps (ESP32-S3-DevKitC-1)
├── hardware/            Reference electronics, wiring, bill of materials
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
| [Safety](docs/SAFETY.md) | Homing, E-stop, fault handling |
| [Arduino IDE](docs/ARDUINO_IDE.md) | Building without PlatformIO |

> The in-depth guides above are currently written in French; this README is the
> English overview and entry point.

The original specifications are the three markdown files at the repository root
(in French): the full requirements (`cahier des charges.md`), the string/fret CC
selection spec, and the SysEx capability protocol.

---

## Project status

**What is done and verified in CI:**

- Complete, unit-tested logic core (129 native tests, 0 failures).
- Real ESP32-S3 firmware build (PlatformIO) and a fast host compile-check.
- Every shipped instrument profile is loaded through the real firmware parser.
- Web interface (vanilla JS, no build step) and JSON profiles validated.

**Not yet done — hardware validation.** The firmware has **not** been run against
a physical instrument. STEP timing on a logic analyzer, six simultaneous axes,
MIDI endurance, and faulty/missing/inverted sensor behavior still need a real
test bench. Treat the current state as **ready for bench bring-up**, not for an
unattended, fully-strung instrument under power.

Known limitations and roadmap are listed at the bottom of
[`docs/SAFETY.md`](docs/SAFETY.md) and throughout the docs.

### Safety note

The software emergency-stop is a convenience, **not** a substitute for a hardware
cut of the driver `ENABLE` / motor power. Wire a physical E-stop before putting
motors under load. See [`docs/SAFETY.md`](docs/SAFETY.md).
