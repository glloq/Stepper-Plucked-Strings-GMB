# Web Interface — Stepper-Plucked-Strings-GMB

> Sources: `SPECIFICATION.md` §9, §10, §18, §19, §20 · `STRING_FRET_SELECTION.md` §14–16 · `SYSEX_CAPABILITIES.md` §17–18.
> Related documents: [`ARCHITECTURE.md`](ARCHITECTURE.md) · [`PIN_CONFIGURATION.md`](PIN_CONFIGURATION.md) · [`MIDI_PROTOCOL.md`](MIDI_PROTOCOL.md) · [`FIRST_CONFIGURATION.md`](FIRST_CONFIGURATION.md).

The Web interface lets a beginner configure the instrument without modifying the
source code, from a computer, a tablet or a phone. No dedicated application is
required.

---

## 1. Two interface levels (§9.2)

### Simplified mode (beginner)

Step-by-step wizard, recommended values, automatic pin assignment, wiring
diagrams, test buttons, automatic validation, understandable error messages. By
default it shows only the **green** GPIOs (see
[`PIN_CONFIGURATION.md`](PIN_CONFIGURATION.md)).

### Advanced mode (fine-tuning)

Manual GPIO assignment (including the **yellow** pins, with an explanation),
adjustment of speeds/accelerations/delays, velocity curves, diagnostics, editing
of detailed parameters, JSON import/export.

---

## 2. First-configuration wizard — 9 steps (§10)

| Step | Title | Content |
| ----- | ----- | ------- |
| 1 | **Identification** | name, description, number of strings, instrument type, proposed tuning, max frets (applied to all strings), **capo** |
| 2 | **Board selection** | ESP32 model → available/reserved/recommended GPIOs (`esp32-s3-devkitc-1` profile); plus a **Network** panel: Wi-Fi mode (AP/station), SSID, hostname, AP name |
| 3 | **Automatic assignment** | "Assign pins automatically" button (number of strings, interfaces, board, future USB, diagnostic port, I²C, sensors) |
| 4 | **Mechanical configuration** | per string: axis enabled, vibrating length, transmission, motor wiring polarity (invert direction), **max speed & acceleration** (now in the simplified view), and Advanced geometry; a **jog ±1/±5 mm** control to check the motor direction live; **Copy mechanics to all strings** |
| 5 | **Homing** | per axis: HOME GPIO, active level, homing search direction, **zero offset / rest position (FDC)**; Advanced adds speeds, back-off, timeout, LIMIT GPIO & level; **Home all axes now** and **Copy homing to all** |
| 6 | **Servo calibration** | per servo: source/channel, rest, active, travel/settle, disable at rest; **strum/stroke motion** for strike roles — alternate stroke direction (+ up-stroke pulse), stroke time, minimum strike depth; **engage delay** for a strum lift; a **Test strike** pulse |
| 7 | **Note calibration** | per string a **Fret offset from FDC** (nut position) that shifts every fret; automatic fret computation **or** manual calibration — move the axis and **Capture position** records the live motor position; an **Abs (FDC)** column; **Copy scale + calibration to all** |
| 8 | **Test** | test each motor, sensor, finger, pick, note, string, a chord, the emergency stop |
| 9 | **Validation** | "Valid configuration" or a precise list of problems; no actuator is enabled until the critical errors are fixed |

The per-string steps (4–7) show **one string at a time** via a string-tab strip,
so a 6-string instrument stays navigable. General MIDI parameters (sustain CC,
chord **saturation strategy**, velocity curve…) and a **Playback timing** card
(fixed note-execution delay, finger lead, strum lead) live on the **MIDI** page.

The step-by-step detail is in [`FIRST_CONFIGURATION.md`](FIRST_CONFIGURATION.md).
The computations for steps 4–7 are in [`CALIBRATION.md`](CALIBRATION.md).

---

## 3. Interface pages

### 3.1 Dashboard (§19)

Main page — overall status:

```text
overall state · Wi-Fi connection · MIDI source · active profile ·
strings-ready count · notes playing · active faults ·
temperatures · voltages · STOP button
```

Per string: status (state machine), current note, current fret, motor position,
target position, remaining distance, HOME state, LIMIT state, finger state, pick
state, last fault.

### 3.2 MIDI page — string/fret selection (STRING_FRET_SELECTION §14–16)

**Simplified** screen (§14):

```text
[✓] Enable string/fret selection
System used: [ General-Midi-Boop ]
String CC: [ 20 ]      Fret CC: [ 21 ]
String numbering: [ 1 to 6 ]
String order: [ Normal ]
When CC is absent: [ Choose automatically ]
```

Buttons: Apply preset · Test reception · Send a test · View received values. The
advanced settings (offsets, tables, policies) stay hidden under "Advanced
settings" (see [`MIDI_PROTOCOL.md`](MIDI_PROTOCOL.md) §2).

**Web MIDI monitor (§15)** — real time:

| Time | Channel | Message | Value | Interpretation |
| ----: | ----: | ------- | -----: | -------------- |
| 0 ms | 1 | CC20 | 3 | string 3 |
| 1 ms | 1 | CC21 | 5 | fret 5 |
| 2 ms | 1 | Note On 60 | 100 | string 3, fret 5 |

Also displays: complete / pending / expired selection, invalid value, automatic
allocation used, note/fret mismatch, actual physical string. A button to clear
the log.

**Built-in test tool (§16)** — choose string, fret, MIDI note, velocity,
channel; automatically sends string CC → fret CC → Note On → Note Off after a
chosen duration, and displays each step (CC received, selection validated, axis
moving, position reached, finger pressed, string plucked).

### 3.3 MIDI page — GMB identity and capabilities (SysEx §17–18)

Path: `MIDI > GMB identity and capabilities`.

**Simplified mode (§17.1)**: enabling GMB detection, name, type, instrument
preset, GM program, MIDI channel, "Publish capabilities" and "Test
communication" buttons, status of the last detection. Computed capabilities,
read-only:

```text
Strings: 4 · Frets: 12 · MIDI range: 40 to 76 · Polyphony: 4
String CC: 20 · Fret CC: 21 · Tuning: E2 A2 D3 G3 · Revision: 7
```

**Advanced mode (§17.2)**: enabling blocks 5/6/7, choice of the block 7 version,
polyphony override, continuous range or discrete notes, viewing the announced CCs
and the SysEx bytes, manual sending of each response, sending the notification,
resetting the identifier, exporting the snapshot.

**SysEx tester (§18)**: simulate "Request identity / descriptor / capabilities /
string configuration / Notify a change / Full discovery". For each test: message
sent, message received, decoding of fields, 7-bit validity, length, possible
error, response time. Protocol details in
[`MIDI_PROTOCOL.md`](MIDI_PROTOCOL.md) §3.

### 3.4 MIDI settings (§18)

Global channel, Omni mode, per-string channel, general/per-string transposition,
note range, velocity curve (linear / soft / hard / exponential / custom), Note
Off behavior, sustain pedal, chord grouping delay (default 3 ms), saturation
strategy (see `NoteAllocator`, [`ARCHITECTURE.md`](ARCHITECTURE.md)). Velocity can
act on the pick travel/speed, the attack delay, the plucking profile.

### 3.5 Profiles (§20)

At least **8 profiles**. Functions: create, copy, rename, delete, export, import,
restore, set the startup profile. **JSON** exchange format:

```json
{
  "project": "Stepper-Plucked-Strings-GMB",
  "profileVersion": 1,
  "instrument": { "name": "Ukulele 4 strings", "stringCount": 4 },
  "board": { "profile": "esp32-s3-devkitc-1", "reserveUsb": true, "automaticPinAssignment": true },
  "network": { "mode": "station", "hostname": "gmb-ukulele" },
  "strings": []
}
```

The Wi-Fi password **never** appears in ordinary exports (unless an explicit
option is set).

---

## 4. REST / WebSocket API (`web/` adapter)

> API assumed for the Web layer (module §23 `web/RestApi`, `web/WebSocketStatus`,
> `communication/WebSocketMidi`). It exposes the `Profile` / `PinManager` /
> `SafetyManager` / `GmbSysEx` core described in [`ARCHITECTURE.md`](ARCHITECTURE.md).

### 4.1 REST endpoints

| Method | Endpoint | Role |
| ------- | -------- | ---- |
| `GET` | `/api/status` | overall status + per string (dashboard §19) |
| `GET` | `/api/profile` | active profile (JSON) |
| `PUT` | `/api/profile` | replace the profile (draft → validation → activation) |
| `GET` | `/api/profiles` | list of saved profiles |
| `POST` | `/api/profiles` | create / copy / import a profile |
| `GET` | `/api/board/{id}` | board profile + GPIO capabilities (colors, filtering) |
| `POST` | `/api/pins/auto` | automatic assignment (`PinRequest`) → assignments |
| `POST` | `/api/pins/validate` | pin validation → list of `PinError` |
| `POST` | `/api/panic` | software panic (`SafetyManager::panic`) |
| `POST` | `/api/test/note` | play a test note (string, fret, note, velocity, channel) |
| `POST` | `/api/sysex/request` | simulate a GMB SysEx request → decoded response |
| `GET` | `/api/capabilities` | current capabilities snapshot (read-only) |

### 4.2 WebSocket

| Channel | Role |
| ----- | ---- |
| `WS /ws/midi` | inbound/outbound MIDI stream (MIDI monitor §15, binary WebSocket transport) |
| `WS /ws/status` | real-time dashboard and per-string status (§19) |

Notes:

* `PUT /api/profile` follows the draft → `ProfileValidator` → atomic save →
  `capabilitiesRevision` increment → snapshot rebuild → Block 8 notification flow
  (see [`MIDI_PROTOCOL.md`](MIDI_PROTOCOL.md) §3.7). A draft configuration is
  **never** published.
* `POST /api/pins/auto` and `/api/pins/validate` map directly to
  `PinManager::autoAssign` / `validate` (see [`PIN_CONFIGURATION.md`](PIN_CONFIGURATION.md)).
* `POST /api/panic` and the safety state: see [`SAFETY.md`](SAFETY.md).
