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
* **Native tests** (`firmware/test/`) — the whole core compiled and run with
  `g++ -std=c++17` via `firmware/test/Makefile`, under `-Werror` and again under
  AddressSanitizer + UBSan in CI. The CI badge is the source of truth for the
  count; there is deliberately no hard-coded number here to go stale.

```bash
cd firmware/test && make        # compile the core + the tests, then run them
```

Some code cannot be reached by a native unit test because it is Arduino-gated.
Rather than leave it merely *compiled*, four harnesses exercise it with real
implementations against instrumented stubs:

| Harness | What it proves |
| ------- | -------------- |
| `test/hostcheck/` | `main.cpp` and every ESP32 adapter still compile (types, signatures, ArduinoJson) |
| `test/servobankcheck/` | dual-I²C-bus routing, the controlled and governed parks, `ActuatorResult`, the board→string map |
| `test/stepperbankcheck/` | `hardStop()` force-stops and drops ENABLE while `controlledStopAll()` decelerates and keeps it; refused moves; soft-limit clamping; an axis with no step generator |
| `test/profilecheck/` | every shipped profile through the real parser, the v1→v2 migration, the split-slot round trip |
| `test/boardcheck/` | the JSON board profiles still match `BoardProfile.cpp` |

This separation guarantees that a new MIDI transport or a new board does not
affect the string controller, the allocator, the motion management, or the
mechanical profiles (specification §8.3).

---

## 2. Target tree (§23) and correspondence with the code

Specification §23 describes the complete **target** tree. The table below
places that tree side by side with the modules **actually implemented** in
`firmware/src/core/`.

```text
firmware/src/core/               pure C++17 — no Arduino, unit-tested on a host
├── Types.{h,cpp}                capacity bounds, fret geometry, gamme law
├── app/
│   ├── AppPhase.h               lifecycle phase → the wire labels the UI reads
│   └── Readiness.h              the ready / degraded rule (a string is ready only
│                                if enabled AND un-faulted)
├── board/
│   ├── BoardProfile.{h,cpp}     four board profiles + per-signal pin capability
│   └── PinManager.{h,cpp}       assignment AND validation (§23's PinValidator is
│                                merged in: both need the same BoardProfile)
├── configuration/
│   ├── Profile.{h,cpp}          the single source of truth (schema v2)
│   ├── ProfileValidator.{h,cpp} semantic validation — nothing arms without it
│   ├── ProfileActivation.h      the two-phase profile swap (RAII + timing)
│   ├── DeviceInstrument.h       DeviceConfig / InstrumentProfile split (P1.13)
│   └── ServoStroke.h            velocity → strike-depth maths
├── diagnostics/
│   └── Diagnostics.h            runtime telemetry accumulator (P2.19)
├── gmb/
│   ├── Capabilities.{h,cpp}     capabilities snapshot
│   ├── GmbSysEx.{h,cpp}         SysEx encoder/decoder
│   └── GmbSysExService.{h,cpp}  request handling + rate limiting
├── instrument/
│   ├── InstrumentController.{h,cpp}  orchestration, chord grouping
│   ├── StringController.{h,cpp}      per-string state machine
│   ├── NoteAllocator.{h,cpp}         note → string allocation
│   ├── ActuatorResult.h              Ok / InvalidIndex / Disabled / … (P1.4)
│   ├── ActuatorManager.h             deadline vs staggerable movements (P1.6)
│   └── ServoActivationGovernor.h     the in-rush start governor
├── midi/
│   ├── MidiEvent.h              transport-neutral event + MidiSource tag
│   ├── MidiParser.{h,cpp}       bytes → MidiEvent (running status, SysEx)
│   ├── MidiTransport.h          the interface every transport implements (P1.7)
│   ├── StringFretSelector.{h,cpp}  CC20/CC21 tablature selection
│   └── Velocity.h               velocity curves
├── motion/
│   ├── StepperAxis.{h,cpp}      geometry, mm ↔ steps, soft limits
│   ├── MotionPlanner.{h,cpp}    trapezoidal reference model
│   └── HomingController.{h,cpp} non-blocking homing per axis
├── net/
│   └── UdpSourceGate.h          UDP session posture (P1.11)
├── safety/
│   └── SafetyManager.{h,cpp}    ConfigSafe / PowerOnSafe / Homing / Armed /
│                                Panic / EmergencyStop + the fault log (§23's
│                                FaultManager is merged in)
└── util/
    ├── Debounce.h               input debouncing
    ├── HoldButton.h             BOOT long-press → hotspot
    └── CommandResultRing.h      outcome of the last N web→loop commands

firmware/src/platform/esp32/     the hardware glue — Arduino-gated
├── StepperBank.{h,cpp}          STEP/DIR via the FastAccelStepper hardware engine
│                                (RMT/MCPWM + timer), so pulses are generated
│                                OUTSIDE loop() and are immune to Wi-Fi/web/I²C
│                                latency; hardStop / controlledStopAll
├── ServoBank.{h,cpp}            PCA9685 on two I²C buses + direct-GPIO (LEDC);
│                                hardStop, controlled and governed parks
├── MidiWifi.{h,cpp}             UDP MIDI transport (+ the UDP source gate)
├── MidiDinTransport.h           DIN-5/TRS over UART — complete, inert until a
│                                RX pin is bound
├── MidiUsbTransport.h           native USB-MIDI skeleton (awaits TinyUSB)
├── PlaybackScheduler.h          per-string mechanical FSM (release → move →
│                                press → settle → strike) + the per-axis
│                                endstop scan that runs ahead of it
├── SafetySupervisor.h           arming, homing, hard stop, panic, the runtime
│                                axis-fault path, E-stop polarity
├── CommandDispatcher.h          the web→loop queue, ids and outcome ring
├── Net.{h,cpp}                  Wi-Fi station/AP, forced hotspot, captive
│                                portal, async network survey
├── WebApi.{h,cpp}               REST + WebSocket
└── ProfileStorage.{h,cpp}       LittleFS slots (split device/instrument layout,
                                 atomic temp+bak writes) + NVS for secrets
```

`main.cpp` is what remains: it owns the mechanical state and wires the pieces
together — `setup()` builds and binds, `loop()` runs safety first, then commands,
then the playback tick. Every mutating web request only *enqueues*; the async web
task never touches an actuator.

### The three platform components lifted out of `main.cpp`

`main.cpp` was 1900 lines with the playback FSM, the arming sequence and the
command plumbing inlined in the middle of it. Those three are now named units:

| | Owns | Gets injected |
| --- | --- | --- |
| `PlaybackScheduler` | the per-string `StringSched` state | instrument, steppers, servos, actuator governor, diagnostics, profile; a fault callback and an `actOk` callback |
| `SafetySupervisor` | nothing | every collaborator by pointer, plus `rebuildCaps` / `notifyCaps` / `hardStopCleanup` / `onReady` callbacks |
| `CommandDispatcher` | the FreeRTOS queue, id counter, result ring | one handler callback that runs a drained command |

Two properties of that split are deliberate:

* **The bodies moved verbatim.** Each method aliases its injected collaborators
  back to the historical `g_*` names on its first lines, so the sequencing code
  is byte-identical to what it replaced. This is a mechanical path on a machine
  with 24 V steppers; a refactor is not the place to also "improve" the order in
  which a finger lifts and a carriage moves.
* **The components own no policy.** A refused actuator write goes back to the
  app's central fault path rather than being decided locally, so "what does a
  fault mean for capabilities and arming" still has exactly one answer.

They are Arduino-gated, so they are covered by the host *compile* check, not by
host unit tests. The logic that IS unit-tested is the part that could be got
silently wrong without hardware: `estopAssertedFor()` lives in the pure core
(`core/safety/EstopPolarity.h`) precisely because inverting it makes a healthy
machine refuse to home while a pressed button goes unseen. Everything else here
is to be **re-validated on the bench**, not assumed correct because it builds.

Correspondence notes vs the §23 target tree:

* `PinValidator` is merged into `PinManager::validate()`, and `FaultManager`
  into `SafetyManager` — in both cases the two halves need the same state.
* `gmb/` does not appear in the §23 tree: it realises the
  [`SYSEX_CAPABILITIES.md`](SPEC_INDEX.md) specification.
* The `application/` layer (`Application` / `Scheduler` / `EventBus`) is
  realised, but split by *testability* rather than by name. The host-testable
  kernels live in the pure core (`AppPhase`, `Readiness`, `CommandResultRing`,
  `HoldButton`, `ProfileActivation`, `Diagnostics`, `ActuatorManager`); the parts
  that can only be exercised against real hardware are the three platform
  components above. There is no `EventBus`: with one producer (the loop) and one
  consumer per concern, a bus would be indirection without a subscriber.

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
| `instrument` | `InstrumentInfo` | name, type, GM program, number of strings, capo, transposition |
| `boardIdentifier` / `reserveUsb` / `pins` | — | board, USB reservation, GPIO assignment |
| `estopNormallyClosed` | `bool` | E-stop contact wiring — a NC loop fails safe |
| `network` | `NetworkConfig` | AP/station mode, SSID, hostname, AP name |
| `midi` | `MidiConfig` | channel, Omni, transposition, chord window, velocity curve, pedal, the execution delay and the finger/strum leads |
| `selector` | `SelectorConfig` | string/fret selection (CC20/CC21, mode, timeout, FIFO…) |
| `power` | `PowerConfig` | in-rush governor caps (global, per PCA board, stagger) |
| `pluck` | `PluckConfig` | the plucking gesture and the Note-Off mute behaviour, common to every string |
| `hardware` | `HardwareNotes` | what safety hardware is physically fitted, and the currents the web estimator sizes from — documentation only, drives no runtime behaviour |
| `strings` | `vector<AxisConfig>` | geometry/motor per string |
| `homing` | `vector<HomingConfig>` | homing per axis |
| `servos` | `vector<ServoConfig>` | servos (finger/pluck/strum/strumLift/damper/aux) |
| `profileVersion` | `uint16_t` | schema version (currently 2) — `ProfileStorage::migrate()` upgrades older files explicitly |
| `capabilitiesRevision` | `uint32_t` | revision counter (Block 8 notification) |

The `hardware` block is deliberately inert: it describes the contactor, the
pull-ups and the fuses, which matter *precisely when the firmware cannot act*.
Storing it with the profile means the builder's declarations follow the
instrument across devices and exports.

`Profile::instrumentView()` derives from it an `InstrumentView` shared by the
string/fret selector and the capabilities generator.

---

## 6. Development phases (§24)

| Phase | Objective | Key deliverables |
| ----- | ----- | -------------- |
| **1 — Single-string prototype** | ESP32-S3, Wi-Fi, minimal UI, 1 motor, 1 HOME sensor, 1 finger servo, 1 pluck servo, Wi-Fi MIDI test, complete state machine, panic | state machine, homing, panic |
| **2 — Intuitive configuration** | wizard, board profile, automatic GPIO assignment, conflict validation, motor/servo calibration, JSON import/export | `BoardProfile`, `PinManager`, `Profile`, wizard |
| **3 — Multi-string** | 4 then 6 axes, PCA9685, parallel homing, note allocation, chords, per-string diagnostics | `NoteAllocator`, parallel homing |
| **4 — Advanced playing** | tremolo, damping, sustain pedal, velocity curves, saturation strategies | curves |
| **5 — Dedicated hardware** | schematic, PCB, protections, connectors, hardware shutdown, electrical validation, wiring documentation | `hardware/` |
| **6 — Future communications** | BLE MIDI, USB MIDI, MIDI DIN, wired links | new transports reusing `MidiEvent` |

Phases 1–4 are implemented in software: the core, the ESP32 adapters, the web
interface, and the safety/telemetry work of the audit
([`../AUDIT_REPORT.md`](../AUDIT_REPORT.md)). Phase 5 exists as the reference
circuit, schematics and commissioning procedure in
[`../hardware/`](../hardware/README.md), not yet as a PCB. Phase 6 has its
abstraction in place (`MidiTransport`, with UDP functional and DIN complete but
inert until a RX pin is bound) — see §7.

**None of it has run against a physical instrument.** Everything above is
verified in software; start bench bring-up with
[`../hardware/COMMISSIONING.md`](../hardware/COMMISSIONING.md).

---

## 7. Transport independence

Adding a transport (BLE, USB, DIN, serial, CAN/RS485) must modify neither the
string controller, nor the allocator, nor the motion management, nor the
mechanical profiles (§8.3). This is now enforced by an interface rather than by
convention: `core/midi/MidiTransport.h` captures the per-tick ingestion contract
(`poll` / `events` / `clear` / `source` / `name`), and `main.cpp` feeds the
**same** `InstrumentController` from a list of transports.

| Transport | State |
| --------- | ----- |
| `MidiWifi` (UDP, port 5006) | functional — implements `MidiTransport`, keeps its IP-addressed SysEx back-channel |
| `MidiDinTransport` (DIN-5/TRS over UART) | byte→event logic complete and compile-checked; wired **inert** (`begin(nullptr)`) until a config exposes a RX pin. Reception over a real opto-coupler is unvalidated |
| `MidiUsbTransport` (native USB on the S3) | documented skeleton, awaits TinyUSB |
| BLE | not started |

Each transport stamps `MidiEvent.source`, so diagnostics and routing can tell the
inputs apart. Bring-up and any reply channel stay on the concrete class: UDP needs
a port and addressed replies, USB/DIN/BLE do not. GPIO19/GPIO20 remain reserved by
default for the ESP32-S3 native USB.
