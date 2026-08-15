# MIDI Protocol — Stepper-Plucked-Strings-GMB

> Sources: `SPECIFICATION.md` §8 · `STRING_FRET_SELECTION.md` (full) · `SYSEX_CAPABILITIES.md` (full).
> Code: `firmware/src/core/midi/{MidiEvent.h, StringFretSelector.*}`, `core/gmb/{GmbSysEx.*, Capabilities.*}`.
> Related documents: [`ARCHITECTURE.md`](ARCHITECTURE.md) · [`WEB_INTERFACE.md`](WEB_INTERFACE.md).

This document covers three areas:

1. the Wi-Fi MIDI transport and the internal `MidiEvent` event (§8.2);
2. explicit string/fret selection via CC (the `STRING_FRET_SELECTION.md` spec);
3. the GMB SysEx capabilities protocol (the `SYSEX_CAPABILITIES.md` spec).

---

## 1. Wi-Fi MIDI transport and `MidiEvent` (§8.2)

The transport layer is **separate** from the internal MIDI engine. In the first
version, the Wi-Fi inputs can be:

* binary WebSocket;
* RTP-MIDI;
* a configurable UDP protocol;
* test commands from the Web interface.

### Who sent it: `origin`

`MidiSource` names a **transport**. That was a sufficient key while each transport
had one sender — and it stopped being one as soon as the instrument accepted
several. The default UDP posture accepts any host, so two laptops on the same
Wi-Fi both arrive as `MidiSource::WifiUdp`, and

```text
PC A -> NoteOn  ch1 C4
PC B -> NoteOn  ch1 C4
PC B -> NoteOff ch1 C4
```

is ambiguous: the Note Off can release PC A's note, damping their string and
leaving PC B's pressed with nothing able to release it.

So every event also carries an **origin** — a small id for *the thing that sent
this* (`core/midi/MidiIdentity.h`):

| Origin | Sender |
| ------ | ------ |
| 0 | firmware-internal |
| 1 | DIN-5 / TRS (one cable, one sender) |
| 2 | USB-MIDI (one host) |
| 3 | the web test tools |
| 4–15 | network peers, one per `IP:port` |

A note is identified by `(origin, channel, note)`, and anything scoped to a sender
— pending CC selections, the sustain pedal, the last-valid selection, the reach of
an All Notes Off — by `(origin, channel)`.

#### Recycling an origin is not free

The network table holds 12 peers, so ids get reused. The first version of this
argued that reuse "only confuses two peers with each other, which is the behaviour
of having no ids at all". That was **wrong**, and wrong in the one direction that
leaves a finger pressed on a ringing string:

```text
PC A -> NoteOn  ch1 C4      recorded as (origin 4, ch1, C4)
        ... enough other endpoints appear, A's slot is recycled
PC A -> NoteOff ch1 C4      A is new again: (origin 7, ch1, C4)
                            -> no match. The Note Off is lost.
```

Without ids, A's key was *shared* but *stable*. Recycling changes one sender's
identity between its own Note On and its own Note Off, which the earlier design
could not do — and it does not take 13 simultaneous players: a client that
reconnects on a fresh ephemeral port is a new `IP:port` every time.

Three rules close it, and all three are needed:

* **A talking peer keeps its origin.** Eviction takes the *least recently used*
  slot, never the next one round a ring, so a peer that is mid-note is the last
  one chosen rather than an arbitrary one.
* **Silent slots are reclaimed** after `kPeerIdleTimeoutMs` (10 minutes), so the
  table does not silt up with dead reconnects and start evicting live players.
* **Losing a slot is reported.** `MidiWifi::takeReleasedOrigins()` hands the owner
  every origin that was evicted or expired, and `main.cpp` calls
  `InstrumentController::releaseOrigin()` on each: notes, chord buffer, sustain
  pedal and pending selections, across all 16 of that origin's channels. A note
  that outlives its sender is a stuck note whatever the table size.

`firmware/test/runtimecheck` drives the real peer table for all three, and the
integration tests cover the instrument side.

### CC120 / CC123 are messages, not stop buttons

All Sound Off and All Notes Off act on **the sender's own channel**. They used to
call the instrument-wide panic, so a DIN controller sending CC123 also damped the
Wi-Fi player's strings, dropped their pedal and wiped their pending selections.

An instrument-wide stop is a *safety* action and has its own routes:
`POST /api/panic`, the hardware E-stop, and the panic path in `main.cpp`.

All transports produce a **common internal event**:

```cpp
struct MidiEvent {
    uint32_t timestampUs = 0;
    uint8_t source;    // MidiSource — which TRANSPORT (WifiUdp, Din, Usb, WebUiTest…)
    uint8_t origin;    // which SENDER on it (a cable, or one network IP:port)
    uint8_t type;      // MidiType : NoteOff 0x80, NoteOn 0x90, ControlChange 0xB0, SysEx 0xF0…
    uint8_t channel;   // 0..15 (internal, base 0)
    uint8_t data1;
    uint8_t data2;
    bool isNoteOn()  const;  // NoteOn with velocity > 0
    bool isNoteOff() const;  // NoteOff, OR NoteOn velocity 0 (running status)
    bool isControlChange() const;
};
```

Possible future extensions without modifying the core: BLE MIDI, serial link,
CAN/RS485. GPIO19/GPIO20 remain reserved for native USB.

### 1.1 Physical inputs

Every transport feeds the **same** `InstrumentController`; `MidiEvent.origin`
keeps their SENDERS apart (see above — the transport alone is not enough once one
transport can carry several). Two are built:

| Transport | Class | State | Bound when |
| --------- | ----- | ----- | ---------- |
| Wi-Fi UDP | `MidiWifi` | working, validated | always (UDP port 5006), subject to the source policy below |
| DIN-5 / TRS | `MidiDinTransport` | working, validated | a `MIDI_RX` GPIO is assigned |
| Native USB | `MidiUsbTransport` | built, **not hardware-validated** | the `esp32-s3-usbmidi` image is flashed *and* a host has enumerated it |

DIN MIDI needs no special stack: it is 31250-baud serial. `bindDinMidi()`
(`main.cpp`) opens **UART2** — UART0 is the programming/diagnostic console, and
binding MIDI there would eat the boot log — as **RX only**, so no TX pin is
claimed from the user. It is re-bound after every profile activation, because the
`MIDI_RX` pin can move with a new configuration.

The pin comes from the `MIDI_RX` signal in the GPIO editor
(**Wiring & GPIO → GPIO pins**, `SignalKind::UartRx`). Its rule is deliberately
wider than the E-stop's: any readable, non-strapping pin will do, including the
classic ESP32's input-only 34/35/36/39. A powered MIDI sender can hold the line
either way through a reset, which is exactly what a strapping pin must not see.

With no `MIDI_RX` pin the transport stays inert rather than half-configured, and
`GET /api/status` says so:

```json
"midiSource": "wifiUdp",
"midiTransports": [
  { "name": "wifiUdp", "label": "Wi-Fi (UDP)", "bound": true,  "detail": "UDP port 5006",         "events": 412 },
  { "name": "usb",     "label": "USB-MIDI",    "bound": false, "detail": "not implemented in this build", "events": 0 },
  { "name": "din",     "label": "DIN-5 / TRS", "bound": true,  "detail": "GPIO16, UART2, 31250 baud",     "events": 37 }
]
```

`midiSource` names the transport that most recently delivered a message —
`"none"` before the first one — so "which input is driving this instrument right
now?" has a measured answer. It was once the constant `"wifiUdp"`, then briefly
"the transport with the most messages since boot", which answers a different
question: a lifetime total still says Wi-Fi on a machine that has been on Wi-Fi
all day, however hard you play a DIN cable plugged in five minutes ago. The
lifetime counts are still reported per transport, as `events`.

The **source policy** (`POST /api/midi/source`, Settings → Security) governs the
Wi-Fi transport only. A physical cable is trusted by being plugged in — there is
no sender identity on a DIN line to lock to.

Hardware wiring for the optocoupled DIN/TRS input is in
[`hardware/POWER_AND_SAFETY.md`](../hardware/POWER_AND_SAFETY.md) §4.1 — the
isolation there is mandatory, not a refinement.

### 1.2 Native USB-MIDI — opt-in, and why

`Adafruit_USBD_MIDI` is a `Stream`: it de-packetises the 4-byte USB-MIDI packets
into a plain MIDI byte stream, so the transport is the same three lines as the
DIN one. The cost is not the code, it is the peripheral.

The ESP32-S3 has **one** USB peripheral and two mutually exclusive Arduino modes:

| | `ARDUINO_USB_MODE=1` (default env) | `ARDUINO_USB_MODE=0` (`esp32-s3-usbmidi`) |
| --- | --- | --- |
| USB stack | hardware USB-CDC / JTAG | TinyUSB |
| USB-MIDI | impossible | available |
| Serial console | always there, even if the firmware hangs | emulated CDC — absent while enumerating or wedged |
| Reflash | plain `esptool` | manual BOOT-button entry when it goes wrong |

Losing a console that survives a hang is a real cost during bring-up, so the
default S3 image keeps the hardware CDC and `esp32-s3-usbmidi` is a separate env.
Both are built in CI, so the TinyUSB path cannot rot unnoticed.

**Not hardware-validated.** It compiles; nobody has enumerated it against a host,
and the Adafruit TinyUSB tracker carries known S3-specific enumeration quirks.
Treat the first flash as a bench experiment — confirm the device appears as a
MIDI port *and* that the console still reaches you — before relying on it. Wi-Fi
UDP and DIN are the validated inputs. The classic ESP32 has no native USB at all;
there is nothing to enable there.

---

## 2. Explicit string and fret selection via CC

Code: `core/midi/StringFretSelector.{h,cpp}`.

### 2.1 Purpose and convention

The controller can receive an explicit string and fret indication before a
`Note On`, allowing General-Midi-Boop to transmit a tablature position:

```text
CC20 (string) → CC21 (fret) → Note On
```

Default convention: **CC20 = string number**, **CC21 = fret number**.
Example: `CC20=3`, `CC21=5`, `Note On 60 vel 100` → play note 60 on physical
string 3, fret 5, velocity 100.

No CC number and no value is hard-coded: everything is configurable from the Web
interface.

### 2.2 Selection modes

| Mode | `SelectionMode` | Behavior |
| ---- | --------------- | -------- |
| **Automatic** | `Automatic` (0) | ignores CCs, always allocates automatically; compatible with standard MIDI files |
| **Explicit** | `Explicit` (1) | string and fret imposed by the CCs received before the Note On |
| **Hybrid** | `Hybrid` (2) | **default**: CCs if complete, valid and playable, otherwise fall back to automatic allocation |

Hybrid mode is the default because it reads standard MIDI files, enriched files,
General-Midi-Boop commands, and notes from the UI all at once.

### 2.3 General-Midi-Boop preset (`applyGmbPreset()`)

| Parameter | Value |
| --------- | ----: |
| Explicit selection enabled | yes |
| String CC | 20 |
| Fret CC | 21 |
| First string | value 1 |
| First fret | value 0 |
| String / fret offset | 0 / 0 |
| Consumption mode | next note |
| Selection per MIDI channel | yes |
| Fallback allocation | automatic |
| Preparation as soon as CCs are received | yes |

The maxima are adapted to the active profile: string CC 1…number of strings, fret
CC 0…maximum fret of the instrument.

### 2.4 Settings (`SelectorConfig`)

```cpp
struct SelectorConfig {
    bool enabled = true;
    SelectionMode mode = SelectionMode::Hybrid;
    bool perMidiChannel = true;
    uint32_t selectionTimeoutMs = 100;   // range 5..2000 ms
    bool prepareOnCompleteSelection = true;
    uint16_t queueDepth = 32;            // >= 16
    StringSelectionConfig string;        // ccNumber=20, min=1, max=stringCount, offset, numbering, reverseOrder, mapping[]
    FretSelectionConfig fret;            // ccNumber=21, min=0, max=fret, offset, invalidValuePolicy
    NotePositionPolicy notePositionPolicy = CcPriorityWithWarning;
    InvalidValuePolicy missingSelectionPolicy = AutomaticFallback;
    InvalidValuePolicy expiredSelectionPolicy = AutomaticFallback;
};
```

### 2.5 Value transformation

* **String**: `logical string = CC value + offset`, then validated, clamped to
  range, converted to the internal (0-based) index and associated with an axis.
  The numbering (`ZeroBased`/`OneBased`), the order (normal/reversed) and a
  custom `mapping[]` table (logical index → physical axis) are applied.
  `mapStringValue(rawValue)` returns the physical axis index or -1.
* **Fret**: `logical fret = CC value + offset`. `mapFretValue(rawValue)` returns
  the fret or -1. **Fret 0 automatically results in: finger raised, no press,
  plucking the open string.**

Normal vs. reversed order example (4 strings):

| CC value | Normal → physical string | Reversed → physical string |
| -------: | -----------------------: | -------------------------: |
| 1 | 1 | 4 |
| 2 | 2 | 3 |
| 3 | 3 | 2 |
| 4 | 4 | 1 |

### 2.6 Validity delay (timeout)

A selection does not stay active indefinitely: `selectionTimeoutMs` (default
100 ms, range 5–2000 ms). If no matching `Note On` arrives within this delay, the
selection is removed (`expire(nowUs)` to be called periodically).

### 2.7 Reliable chord handling — FIFO queue

The firmware does **not** keep only "last string / last fret received": this
method would fail on chords. It uses a **FIFO queue** of selections
(`PendingStringSelection`), depth ≥ 16, recommended 32.

```cpp
struct PendingStringSelection {
    uint8_t midiChannel;
    bool hasString, hasFret;
    uint8_t stringValue;   // physical axis index (already mapped)
    uint8_t fretValue;
    uint32_t receivedAtUs, expiresAtUs;
    bool complete() const { return hasString && hasFret; }
};
```

Example: `CC20=1, CC20=3, CC20=4, CC21=2, CC21=5, CC21=7, NoteOn 42, 55, 64`
reconstructs: selection1 = string 1/fret 2, selection2 = string 3/fret 5,
selection3 = string 4/fret 7; then Note 42 → sel1, Note 55 → sel2, Note 64 → sel3.

### 2.8 Association algorithm

* **String CC** (`onControlChange`): read, apply offset, convert to physical
  string, check the range, **create** a new pending selection.
* **Fret CC** (`onControlChange`): read, offset, check the range, look for the
  **oldest selection without a fret**, add the fret to it, mark it complete.
* **Note On** (`onNoteOn`): look for the **oldest complete selection on the
  channel**, associate the note, validate consistency, remove it from the queue,
  prepare the engine, schedule the press and pluck. Returns:

```cpp
struct NoteResolution {
    bool play;
    ResolveSource source;   // Explicit / Automatic / Rejected
    uint8_t stringIndex, fret;
    uint32_t noteInstanceId;
    std::string warning;    // non-fatal note (e.g. note/fret inconsistency)
};
```

### 2.9 Early preparation

If `prepareOnCompleteSelection` (enabled by default), as soon as a string/fret
pair is complete, the controller can start the mechanical preparation (finger
release, motor movement) **without waiting for the Note On**. The Note On retains
its role as the musical trigger. If the motor has not reached the fret at the
moment of the Note On: the pluck is queued, the motor finishes, the finger
presses, then the pluck executes — **no early plucking**.

### 2.10 Note / string / fret consistency (`NotePositionPolicy`)

Expected note = `open note + fret number + capo + transposition`.

| Policy | Behavior |
| ------ | -------- |
| `CcPriorityWithWarning` (0) | **default**: string/fret from the CCs are used, the difference with the Note On is only reported |
| `NotePriority` (1) | the fret is recalculated from the MIDI note |
| `Strict` (2) | the note is rejected if the information is inconsistent |

### 2.11 Note Off handling

The actual assignment of each Note On is stored. The Note Off does **not** use the
last CC value received: it retrieves the recorded assignment (`onNoteOff` returns
the `ActiveNote`).

```cpp
struct ActiveNote {
    uint8_t midiChannel, midiNote, stringIndex, fret;
    uint32_t noteInstanceId;
};
```

This releases the correct string, handles multiple simultaneous strings and
chords, and prevents a new selection from modifying an already active note. For
repeated notes of the same pitch, a stack of instances is used.

### 2.12 Invalid values (`InvalidValuePolicy`)

| Policy | Behavior |
| ------ | -------- |
| `Reject` (0) | reject the command |
| `Clamp` (1) | clamp to the allowed range |
| `AutomaticFallback` (2) | **default**: automatic allocation + warning |
| `LastValid` (3) | use the last valid value |

Invalid cases: nonexistent string, fret beyond capacity, disabled axis, faulted
string, uncalibrated fret, expired selection, incomplete CC pair.

### 2.13 Configuration validation (§18)

* String CC and fret CC between 0 and 119 (**CC120–127** = channel mode messages,
  never offered — cf. `kMaxAssignableCc = 119`);
* two different CC numbers;
* no conflict with another configured function;
* string/fret ranges compatible with the profile;
* sufficient queue depth (≥ 16);
* nonzero validity delay.

CC20 and CC21 are presented as recommended choices.

### 2.14 JSON profile (§17)

```json
{
  "stringFretSelection": {
    "enabled": true,
    "mode": "hybrid",
    "preset": "general-midi-boop",
    "perMidiChannel": true,
    "selectionTimeoutMs": 100,
    "prepareOnCompleteSelection": true,
    "queueDepth": 32,
    "string": { "ccNumber": 20, "minimum": 1, "maximum": 6, "offset": 0,
                "numbering": "oneBased", "reverseOrder": false, "mapping": [0,1,2,3,4,5] },
    "fret":   { "ccNumber": 21, "minimum": 0, "maximum": 24, "offset": 0,
                "invalidValuePolicy": "automaticFallback" },
    "validation": { "notePositionPolicy": "ccPriorityWithWarning",
                    "missingSelectionPolicy": "automaticAllocation",
                    "expiredSelectionPolicy": "automaticAllocation" }
  }
}
```

The Web MIDI monitor (§15) and the test tool (§16) are described in
[`WEB_INTERFACE.md`](WEB_INTERFACE.md).

---

### 2.x Order of operations in the CC mapping

A selection CC value is turned into a physical axis / fret in this order, and the
order is part of the contract:

1. **`logical = CC value + offset`**;
2. validate `logical` against `[minimum, maximum]` — the bounds apply to the
   LOGICAL value, so configuring an offset moves which raw CC values are accepted
   (that is what an offset is for);
3. drop the one-based bias (`numbering`);
4. apply `reverseOrder`;
5. apply the custom `mapping` table.

Validating the raw value before applying the offset shifts the accepted band by
the offset: the configuration then rejects exactly the values it was set up to
accept. `GMB.decodeStringCc` / `decodeFretCc` in the web interface mirror this
order, and both sides are unit-tested against it.

An **out-of-range value still fills its slot**, marked invalid. Dropping it would
leave the other half of the selection orphaned, and the next unrelated CC would
complete it — playing a note on a string nobody selected. The invalid pair is
resolved by `invalidValuePolicy` at Note On instead.

When several complete selections are queued for one channel, the oldest
**non-expired** one wins. An expired entry is only used as a fallback (through
`expiredSelectionPolicy`), so a stale selection never shadows a newer valid one
sitting behind it.

## 3. GMB SysEx protocol

Code: `core/gmb/GmbSysEx.{h,cpp}` (encoder/decoder) and `core/gmb/Capabilities.{h,cpp}`
(snapshot construction). The service is **transport-independent**: it only
manipulates complete MIDI byte buffers.

### 3.1 Header

```text
F0 7D 00 <bloc> <direction> ... F7
```

| Byte | Function |
| ---- | -------- |
| `F0` | SysEx start (`kStart`) |
| `7D` | experimental/educational SysEx identifier (`kManufacturer`) |
| `00` | General-Midi-Boop identifier (`kGmbId`) |
| `<bloc>` | information type (1 / 0x10 / 0x11 for v2; 5/6/7/8 deprecated) |
| `<direction>` | `00` request, `01` response, `02` spontaneous notification |
| `F7` | SysEx end (`kEnd`) |

Maximum message size: `kMaxMessage = 512` bytes.

### 3.2 Implemented blocks

The current General-Midi-Boop controller speaks **protocol v2**: it asks for the
handshake and then reads one JSON **descriptor**, rather than parsing a set of
fixed binary blocks. The v1 blocks are still served for older hosts.

| Block | `SysExBlock` | Role |
| ----- | ------------ | ---- |
| 1 | `Identity` | **v2 handshake** (24 bytes) — the reply to a block-1 request |
| 0x10 | `DescriptorTransfer` | **v2 JSON descriptor**, segmented (200 payload bytes per chunk) |
| 0x11 | `ChangeNotification` | **v2** spontaneous "something changed" notice |
| 5 | `Descriptor` | deprecated fixed instrument description |
| 6 | `Capabilities` | deprecated fixed capabilities |
| 7 | `StringConfig` | deprecated fixed string configuration (v1 + v2 layouts) |
| 8 | `Notification` | deprecated v1 change notification |

### 3.3 Block 1 — v2 handshake

Request: `F0 7D 00 01 00 F7`. Response (exactly 24 bytes):

```text
F0 7D 00 01 01 <proto_ver=02> <instance_id[5]> <firmware[3]>
               <descriptor_size[3] LE> <revision[5] LE> <flags> F7
```

* `descriptor_size` is the byte length of the JSON descriptor. **Zero means
  level 0** (no descriptor, fall back to the fixed blocks); non-zero means level
  1 and tells the host how much to expect.
* `revision` is the capabilities revision — the host re-reads the descriptor only
  when this changes.
* `flags`: bit 0 = the descriptor is also available over HTTP
  (`GET /gmb/descriptor.json`), bit 1 = the device pushes block 0x11 on change.
  This firmware announces `0x03`.
* `instance_id` is stable across restarts (derived from the ESP32 MAC).

### 3.3b Blocks 0x10 / 0x11 — descriptor and change notice

A descriptor request (`F0 7D 00 10 00 <index[2]> F7`) returns one segment:

```text
F0 7D 00 10 01 <total_chunks[2] LE> <chunk_index[2] LE> <payload…> F7
```

The payload is 7-bit ASCII, up to 200 bytes per chunk; concatenating the chunks
in index order reproduces the JSON byte for byte. The whole document is also
served over HTTP at `GET /gmb/descriptor.json`, which is one request instead of a
segmented transfer.

The descriptor is built by `core/gmb/GmbDescriptor.{h,cpp}` from the same
`CapabilitySnapshot` as the SysEx path, so the two can never disagree. It carries
the device name/model, the playable notes (range or discrete list), polyphony
with a `one_note_per_voice` constraint, the announced CCs, **one voice per
physical carriage** (each with its own open note and reach), and a `physical`
block with the string count, per-string frets, capo, tuning and — only when
selection is actually enabled — the selection CC configuration.

The change notice is 12 bytes: `F0 7D 00 11 02 <revision[5] LE> <flags> F7`,
with flag bit 1 = *instruments changed*. The host uses it purely as a cue to
re-read the handshake and compare revisions.

> **Announced tuning.** `tuning[i]` and `frets_per_string[i]` describe the **same
> physical string** — the tuning is in physical order, never sorted, because the
> descriptor pairs them to build voice *i*. It also folds the global transposes
> into the announced open pitch, so `tuning + capo` reproduces the announced
> playable range. A re-entrant instrument (ukulele G4 C4 E4 A4, 5-string banjo)
> is exactly the case where sorting would misdescribe every voice.

### 3.4 Block 5 — Descriptor (single instrument)

Request: `F0 7D 00 05 00 F7`. Response:

```text
F0 7D 00 05 01 01 01 <channel> <gm_program> <type_id> F7
```

Nylon guitar example (GM 24 = `0x18`, type `0x04`): `F0 7D 00 05 01 01 01 01 18 04 F7`.

### 3.5 Block 6 — Capabilities

Request: `F0 7D 00 06 00 <channel> F7`. Response (`encodeCapabilities`):

```text
F0 7D 00 06 01
<version> <channel> <gm_program> <type_id> <sub_type> <notes_mode>
<note_min> <note_max> <polyphony>
<nombre_notes_discretes> <notes_discretes...>
<nombre_cc> <cc_supportes...>
<longueur_nom> <nom...>
F7
```

#### Playable range (§5) — automatic calculation

`buildSnapshot()` does not enter the range manually: for each active string,
`min note = open note + capo + transpose`, `max note = min note + max frets of the
string`. The **union** of all playable notes is built (bounded 0–127).

* **Continuous range mode** (`notes_mode = 0`): if **all** notes between min and
  max are playable on at least one string. The block carries `note_min`,
  `note_max`, `nombre_notes_discretes = 0`.
* **Discrete notes mode** (`notes_mode = 1`): if some intermediate notes are not
  playable on any string (unusual tunings, few frets, uneven frets, disabled
  strings…). The exact list of playable notes is transmitted.

#### Polyphony (§6)

`polyphony = number of active and functional strings` by default, or a custom
value (`polyphonyOverride ≥ 0`). Examples: 6 strings with individual picks → 6;
6 strings with constraints limiting to 4 → configured to 4.

#### Announced controllers (§7)

Only the CCs that are **actually enabled** are announced, sorted:

| CC | Function | Condition |
| -: | -------- | --------- |
| 7 | volume | always |
| 11 | expression | always |
| String CC | string selection | if `selector.enabled` (configured number, e.g. 24) |
| Fret CC | fret selection | if `selector.enabled` (e.g. 25) |
| 64 | sustain | if `midi.sustainPedal` |
| 120 | immediate sound off | always |
| 123 | all notes off | always |

A disabled CC is not announced.

### 3.6 Block 7 — String configuration

Request: `F0 7D 00 07 00 <channel> F7`.

#### Version 1 (GMB-compatible) — `encodeStringConfigV1`

```text
F0 7D 00 07 01
01 <channel> <string_count> <fret_count> <is_fretless> <capo>
<cc_active> <cc_string> <cc_fret>
<tuning...>
F7
```

`is_fretless = 0` (always for this project). Tuning transmitted in
**low → high** order (GMB order). V1 does not transmit: frets per string, CC
min/max, offsets, reversed order, custom table, selection mode.

#### Version 2 (extension) — `encodeStringConfigV2`

```text
F0 7D 00 07 01
02 <channel> <string_count> <fret_count_global> <is_fretless> <capo>
<cc_active> <cc_string> <cc_fret>
<cc_string_min> <cc_string_max> <cc_string_offset_encode>
<cc_fret_min> <cc_fret_max> <cc_fret_offset_encode>
<selection_mode> <string_order>
<tuning[num]> <frets_per_string[num]> <string_mapping[num]>
F7
```

* **Selection mode**: 0 auto / 1 explicit / 2 hybrid.
* **String order**: 0 normal / 1 reversed / 2 custom mapping.
* **Signed offsets**: encoded as `transmitted_value = offset + 64` (range
  −64…+63), decoded as `offset = transmitted_value − 64` — to stay within 7 bits.

Compatibility: the firmware responds in **v1** as long as General-Midi-Boop does
not support v2 (`respond(req, snap, useV2=false)` by default).

### 3.7 Block 8 — Change notification

`encodeNotification`:

```text
F0 7D 00 08 02
01 <channel> <config_revision[5]> <modification_flags>
F7
```

* The revision is encoded on **5 bytes, 7-bit big-endian** (35-bit capacity).
* Flags (`ChangeFlag`):

| Bit | `ChangeFlag` | Meaning |
| --: | ------------ | ------- |
| 0 | `kIdentityChanged` | identity changed |
| 1 | `kDescriptorChanged` | descriptor changed |
| 2 | `kCapabilitiesChanged` | general capabilities changed |
| 3 | `kStringConfigChanged` | string configuration changed |
| 4 | `kCcMappingChanged` | CC mapping changed |
| 5 | `kRestartRequired` | restart or new homing required |
| 6 | reserved | — |

The notification does not carry all the capabilities: it prompts GMB to restart
its discovery (Block 1 → 6 → 7, and 5 if necessary). GMB only replaces the
configuration if all responses are valid; otherwise the old one stays active with
a warning.

### 3.8 Revision number (§13)

`capabilitiesRevision` (in `Profile`) is incremented on each valid save,
persistent, included in the Block 8 notification, displayed in the UI, and serves
to avoid unnecessary refreshes. A simple restart without modification does not
necessarily increment it.

### 3.9 Immutable snapshot (§19)

```cpp
struct CapabilitySnapshot {
    uint32_t revision;
    DeviceIdentity identity;
    InstrumentDescriptor descriptor;
    InstrumentCapabilities capabilities;
    StringInstrumentConfig stringConfig;
    uint32_t generatedAtMs;
    bool valid;
};
CapabilitySnapshot buildSnapshot(const Profile& p, int polyphonyOverride = -1);
```

A SysEx response uses a **single** snapshot: a modification during transmission
can never mix two versions of the profile. A draft configuration is **never**
published — only the validated and activated profile is.

### 3.10 Protocol safety (§20)

`isWellFormed()` and `parseRequest()` apply:

* header (`F0 7D 00`) and trailer (`F7`) verification;
* minimum length 6 bytes, maximum 512;
* all internal bytes limited to 7 bits (rejected if bit 0x80 is set);
* blocks 6 and 7: channel byte required before `F7`;
* unknown directions ignored; unknown blocks ignored (`respond` returns `{}`);
* no dynamic allocation during processing, rate-limited responses, nonexistent
  channels rejected;
* **never** transmits the Wi-Fi password or sensitive data.

Unknown SysEx messages are ignored without affecting musical operation.

### 3.11 Transport independence (§21)

```text
Wi-Fi MIDI ─┐  (built)
MIDI DIN ───┤  (built)
USB MIDI ───┼──► MidiMessageRouter ─► GmbSysExService
BLE MIDI ───┤  (future)
serial ─────┘  (future)
```

Future Bluetooth/wired versions reuse exactly the same blocks, encoder, decoder,
snapshot and tests.

One asymmetry today: a SysEx **reply** is addressed to its sender's IP, so
request/response still runs on the concrete UDP transport. Notes and CCs arrive
on any transport; a DIN cable can play the instrument but cannot yet interrogate
its capabilities. Each transport will get its own reply path as it lands.

### 3.12 Initial compatibility (§22)

First version: Block 1 v1, Block 5 v1 (recommended), Block 6 v1, Block 7 v1.
Later coordinated extension: Block 7 v2 + Block 8. As long as Block 8 is not
supported on the GMB side, the interface provides a "Re-announce capabilities"
button.
