# Architecture — Stepper-Plucked-Strings-GMB

> Reference document: [`SPEC_INDEX.md`](SPEC_INDEX.md) (source: `SPECIFICATION.md` §23, §24).
> Related documents: [`PIN_CONFIGURATION.md`](PIN_CONFIGURATION.md) · [`MIDI_PROTOCOL.md`](MIDI_PROTOCOL.md) · [`WEB_INTERFACE.md`](WEB_INTERFACE.md) · [`CALIBRATION.md`](CALIBRATION.md) · [`SAFETY.md`](SAFETY.md)

This document describes the software organization of the firmware, how the
modules from the specification (§23) map to the code actually implemented
in `firmware/src/core/*`, the end-to-end data flow, the generation of the
capabilities snapshot, the "pure core + platform adapters + native tests"
strategy, and the development phases (§24).

---

## 1. Strategy: pure core, platform adapters, native tests

The algorithmic core of the firmware is written in **pure C++17**, without any
Arduino or ESP-IDF dependency. This constraint is explicit in the code:

```cpp
// Types.h — This header is pure C++17 with no Arduino / ESP-IDF dependency so
// that the whole algorithmic core can be unit-tested natively on a host with g++.
```

Consequences:

* **Pure core** (`firmware/src/core/`) — business logic testable on a PC: board
  profiles, pin management, string/fret selection, note allocation, per-string
  state machine, motor geometry, homing, GMB capabilities/SysEx, safety. No
  direct hardware access.
* **Platform adapters** (`firmware/src/platform/esp32/`) — concrete
  implementations that connect the core to the ESP32-S3 hardware:
  `StepperBank` generates the steps via the **FastAccelStepper hardware engine**
  (RMT/MCPWM + timer), thus **outside `loop()`** — the core's `MotionPlanner`
  remains the reference trapezoidal model tested on a PC; `ServoBank` drives the
  PCA9685 **and** the servos on direct GPIO (14-bit LEDC); `Net` (non-blocking
  Wi-Fi), `WebApi` (REST + WebSocket), `MidiWifi` (transport), `ProfileStorage`
  (LittleFS + NVS for secrets). These layers consume the core without modifying
  it.
* **Native tests** (`firmware/test/`) — 86 unit tests compiled and run
  with `g++ -std=c++17` via `firmware/test/Makefile`, covering the 8 modules of
  the core (`test_board`, `test_selector`, `test_allocator`, `test_motion`,
  `test_string_fsm`, `test_profile`, `test_sysex`, + `test_main`).

```bash
cd firmware/test && make        # compile the core + the tests, then run them
```

This separation guarantees that a new MIDI transport or a new board does not
affect the string controller, the allocator, the motion management, or the
mechanical profiles (specification §8.3).

---

## 2. Target tree (§23) and correspondence with the code

Specification §23 describes the complete **target** tree. The table below
places that tree side by side with the modules **actually implemented** in
`firmware/src/core/`.

```text
firmware/                        Specification §23             Implemented (core/)
├── application/
│   ├── Application              orchestration                 (adapter, upcoming)
│   ├── Scheduler                non-blocking scheduling        (adapter, upcoming)
│   └── EventBus                 event bus                      (adapter, upcoming)
├── board/
│   ├── BoardProfile             board profiles                core/board/BoardProfile.{h,cpp}
│   ├── PinManager               pin assignment                core/board/PinManager.{h,cpp}
│   └── PinValidator             conflict validation           PinManager::validate() (merged)
├── communication/
│   ├── WifiManager              Wi-Fi AP/station              (adapter, upcoming)
│   ├── MidiTransport            MIDI transport                (adapter, upcoming)
│   ├── WebSocketMidi            MIDI over WebSocket           (adapter, upcoming)
│   └── FutureTransports         BLE/USB/DIN…                  MidiSource enum (core/midi)
├── midi/
│   ├── MidiParser               parsing bytes → MidiEvent     core/midi/MidiEvent.h
│   ├── MidiRouter               routing                       (adapter, upcoming)
│   └── MidiEventQueue           event queue                   (adapter, upcoming)
│   └── (string/fret selection)  CC20/CC21 tablature           core/midi/StringFretSelector.{h,cpp}
├── instrument/
│   ├── InstrumentController     instrument orchestration      (adapter, upcoming)
│   ├── StringController         per-string state machine      core/instrument/StringController.{h,cpp}
│   ├── NoteAllocator            note allocation               core/instrument/NoteAllocator.{h,cpp}
│   └── SharedStrummer           shared strumming              (phase 4, upcoming)
├── motion/
│   ├── StepperAxis              geometry/conversion mm↔steps  core/motion/StepperAxis.{h,cpp}
│   ├── MotionPlanner            trapezoidal profile (accel)   core/motion/MotionPlanner.{h,cpp}
│   └── HomingController         non-blocking homing           core/motion/HomingController.{h,cpp}
├── actuators/
│   ├── ServoManager             PCA9685                       ServoConfig (core/configuration)
│   ├── FingerActuator           finger servo                  ServoConfig function="finger"
│   ├── PluckActuator            pluck servo                   ServoConfig function="pluck"
│   └── DamperActuator           damper                        ServoConfig function="damper"
├── configuration/
│   ├── Profile                  profile (source of truth)     core/configuration/Profile.{h,cpp}
│   ├── ProfileValidator         validation                    core/configuration/ProfileValidator.{h,cpp}
│   └── ProfileStorage           NVS persistence               (adapter, upcoming)
├── safety/
│   ├── SafetyManager            safe states / panic / E-stop  core/safety/SafetyManager.{h,cpp}
│   └── FaultManager             fault log                     SafetyManager::faults() (merged)
├── diagnostics/
│   ├── Logger                   logging                       (adapter, upcoming)
│   └── DiagnosticService        diagnostics                   (adapter, upcoming)
├── gmb/                         (GMB SysEx protocol, §below)
│   ├── Capabilities             capabilities snapshot         core/gmb/Capabilities.{h,cpp}
│   └── GmbSysEx                 SysEx encoder/decoder         core/gmb/GmbSysEx.{h,cpp}
└── web/
    ├── WebServer                HTTP server                   (adapter, upcoming)
    ├── RestApi                  REST API                      (adapter, cf. WEB_INTERFACE.md)
    └── WebSocketStatus          real-time status              (adapter, upcoming)
```

Correspondence notes:

* `PinValidator` (§23) is merged into `PinManager::validate()` — validation
  and assignment share the same `BoardProfile`.
* `FaultManager` (§23) is merged into `SafetyManager` (`recordFault()` /
  `faults()`).
* The `gmb/` module does not appear explicitly in the §23 tree: it realizes
  the [`SYSEX_CAPABILITIES.md`](SPEC_INDEX.md) specification.
* Entries marked "adapter, upcoming" are platform layers or modules from later
  phases that will consume the core.

---

## 3. Main data flow

A MIDI transport produces a single internal `MidiEvent` (specification §8.2),
and everything else in the firmware never depends on how the bytes arrived.

```text
Transport (WebSocket / RTP-MIDI / UDP / Web test / future BLE/USB/DIN)
        │  decoding
        ▼
MidiEvent { timestampUs, source, type, channel, data1, data2 }
        │
        ├──► SysEx (F0 …) ─────────────► GmbSysEx  ──► CapabilitySnapshot ──► response
        │
        ▼
MidiRouter (routing by channel / Omni)
        │
        ▼
StringFretSelector          (explicit string/fret selection CC20/CC21, FIFO)
   ├─ onControlChange()      queues pending string/fret selections
   ├─ onNoteOn() ──► NoteResolution { play, source, stringIndex, fret, instanceId }
   └─ onNoteOff() ──► ActiveNote (releases the string actually used)
        │
        │  (Automatic / Hybrid mode without a valid CC)
        ▼
NoteAllocator               (chooses the best string, groups chords,
                             applies the saturation strategy)
        │  Allocation { stringIndex, fret }
        ▼
StringController[c]          (non-blocking state machine, 1 per string)
   DISABLED → HOMING → IDLE → RELEASING_FINGER → MOVING →
   PRESSING_FINGER → SETTLING → READY_TO_PLUCK → PLUCKING →
   SUSTAINING → DAMPING (→ IDLE)     |  CANCELLING  |  FAULT
        │                                   │
        ▼                                   ▼
StepperAxis / HomingController        ServoManager (PCA9685)
   (mm ↔ steps, fret positions)          finger / pluck / damper
```

Key points of the flow:

* **Common event.** `MidiEvent` (see [`MIDI_PROTOCOL.md`](MIDI_PROTOCOL.md))
  handles the MIDI subtleties (`isNoteOff()` treats a Note On with velocity 0 as
  a Note Off in running status).
* **Selection before allocation.** In `Explicit`/`Hybrid` mode,
  `StringFretSelector` enforces the string/fret; in `Automatic` mode or as a
  fallback, `NoteAllocator` decides. Details in
  [`MIDI_PROTOCOL.md`](MIDI_PROTOCOL.md).
* **Command identifier.** Each `noteOn(fret)` returns a fresh `commandId`;
  any deferred action tagged with an old id is ignored. This prevents a
  pluck after a Note Off, a delayed press, the execution of a stale position,
  or an attack after a panic (specification §16).
* **Reliable Note Off.** The actual assignment of a Note On is memorized
  (`ActiveNote`) to release the correct string, even in a chord or with repeated
  notes.

---

## 4. Capabilities snapshot flow (GMB SysEx)

The active profile is **the single source of truth**. The capabilities
announced to General-Midi-Boop are reconstructed from this profile, never
hard-coded.

```text
Web interface edits a draft
        │
        ▼
ProfileValidator (full validation)
        │  valid
        ▼
Atomic save + capabilitiesRevision increment
        │
        ▼
buildSnapshot(Profile) ──► CapabilitySnapshot (immutable)
        │   { revision, identity, descriptor, capabilities, stringConfig, valid }
        ▼
GmbSysEx::respond(request, snapshot)   (one response = a single snapshot)
        │
        ▼
MIDI transport ──► General-Midi-Boop updates the instrument
        ▲
        └── Block 8 (notification) prompts GMB to restart discovery
```

`buildSnapshot()` (`core/gmb/Capabilities.cpp`) automatically computes the
playable range (union of the notes of all active strings), continuous or
discrete-notes mode, polyphony (number of active strings or overload), and the
list of CC actually enabled. A snapshot is **immutable**: a config change
during sending cannot mix two versions of the profile. See the full protocol
in [`MIDI_PROTOCOL.md`](MIDI_PROTOCOL.md#3-protocole-sysex-gmb).

---

## 5. The profile, backbone of the configuration

`core/configuration/Profile.h` aggregates the entire configuration:

| Field | Type | Role |
| ----- | ---- | ---- |
| `instrument` | `InstrumentInfo` | name, type, GM program, number of strings, capo, transposition, pluck mode |
| `boardIdentifier` / `reserveUsb` / `pins` | — | board, USB reservation, GPIO assignment |
| `network` | `NetworkConfig` | AP/station mode, SSID, hostname, static IP |
| `midi` | `MidiConfig` | channel, Omni, transposition, chord window, velocity curve, pedal |
| `selector` | `SelectorConfig` | string/fret selection (CC20/CC21, mode, timeout, FIFO…) |
| `strings` | `vector<AxisConfig>` | geometry/motor per string |
| `homing` | `vector<HomingConfig>` | homing per axis |
| `servos` | `vector<ServoConfig>` | servos (finger/pluck/damper/aux) |
| `capabilitiesRevision` | `uint32_t` | revision counter (Block 8 notification) |

`Profile::instrumentView()` derives from it an `InstrumentView` shared by the
string/fret selector and the capabilities generator.

---

## 6. Development phases (§24)

| Phase | Objective | Key deliverables |
| ----- | ----- | -------------- |
| **1 — Single-string prototype** | ESP32-S3, Wi-Fi, minimal UI, 1 motor, 1 HOME sensor, 1 finger servo, 1 pluck servo, Wi-Fi MIDI test, complete state machine, panic | state machine, homing, panic |
| **2 — Intuitive configuration** | wizard, board profile, automatic GPIO assignment, conflict validation, motor/servo calibration, JSON import/export | `BoardProfile`, `PinManager`, `Profile`, wizard |
| **3 — Multi-string** | 4 then 6 axes, PCA9685, parallel homing, note allocation, chords, per-string diagnostics | `NoteAllocator`, parallel homing |
| **4 — Advanced playing** | shared strumming, tremolo, damping, sustain pedal, velocity curves, saturation strategies | `SharedStrummer`, curves |
| **5 — Dedicated hardware** | schematic, PCB, protections, connectors, hardware shutdown, electrical validation, wiring documentation | `hardware/` |
| **6 — Future communications** | BLE MIDI, USB MIDI, MIDI DIN, wired links | new transports reusing `MidiEvent` |

The current state of the repository covers the **algorithmic core** of phases 1
to 3 (`core/*` modules + 86 native tests). The platform adapters and the Web
interface constitute the remaining layers.

---

## 7. Transport independence

Adding a transport (BLE, USB, DIN, serial, CAN/RS485) must modify neither the
string controller, nor the allocator, nor the motion management, nor the
mechanical profiles (§8.3). All transports:

1. decode the bytes into `MidiEvent`;
2. forward complete MIDI bytes to the router;
3. reuse exactly the same blocks, encoder, decoder, snapshot, and tests
   for the GMB SysEx (SysEx spec §21).

GPIO19/GPIO20 remain reserved by default for the ESP32-S3 native USB.
