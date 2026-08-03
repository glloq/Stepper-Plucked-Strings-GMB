# Automatic capabilities communication via SysEx

## 1. Objective

Stepper-Plucked-Strings-GMB must automatically communicate its capabilities to General-Midi-Boop.

The announced information must be generated from the active profile stored in the ESP32.

It must not be hard-coded in the firmware.

The system must communicate in particular:

* the device identity;
* the instrument name;
* the firmware version;
* the MIDI channel;
* the instrument type;
* the General MIDI program;
* the number of strings;
* the tuning;
* the number of frets;
* the actually playable note range;
* the polyphony;
* the supported MIDI controllers;
* the CCs used to select the string and the fret;
* the presence of the explicit selection mode;
* the configuration revision.

---

# 2. General principle

```text
Configuration from the Web interface
                │
                ▼
Profile validation
                │
                ▼
Atomic save
                │
                ▼
Capabilities generation
                │
                ▼
Publication via SysEx
                │
                ▼
General-Midi-Boop updates the instrument
```

The active profile must be the single source of truth.

The SysEx data must be rebuilt automatically after:

* startup;
* loading a profile;
* changing the tuning;
* changing the number of strings;
* changing the number of frets;
* changing the MIDI channel;
* changing the string/fret CCs;
* changing the polyphony;
* changing the instrument type or name.

---

# 3. General-Midi-Boop protocol used

All messages must use the header:

```text
F0 7D 00 <bloc> <direction> ... F7
```

| Octet         | Function                                |
| ------------- | --------------------------------------- |
| `F0`          | SysEx start                             |
| `7D`          | experimental/educational SysEx identifier |
| `00`          | General-Midi-Boop identifier            |
| `<bloc>`      | information type                        |
| `<direction>` | request, response or notification       |
| `F7`          | SysEx end                               |

Direction values:

```text
00 = request
01 = response
02 = proposed spontaneous notification
```

The value `02` constitutes an extension to be added jointly in General-Midi-Boop.

---

# 4. Blocks to implement

## 4.1 Block 1 — Identity

Block 1 is mandatory.

### Request

```text
F0 7D 00 01 00 F7
```

### Response

```text
F0 7D 00 01 01
<version>
<device_id[5]>
<device_name[32]>
<firmware[3]>
<features[5]>
F7
```

### Announced information

* permanent device identifier;
* name defined in the Web interface;
* major, minor and patch firmware version;
* supported SysEx blocks.

### Feature flags

For a single instrument supporting capabilities and string configuration:

```text
INSTRUMENT_CAPABILITIES = 0x10
STRING_CONFIG           = 0x20

Features                = 0x30
```

If block 5 is also implemented:

```text
INSTRUMENT_DESCRIPTOR   = 0x08

Features                = 0x38
```

### Device identifier

The identifier must be stable after restart.

It may be generated on first startup from:

* the ESP32 hardware identifier;
* a saved random value;
* or a combination of both.

The user should not normally need to modify it.

An advanced button may allow this identifier to be regenerated.

---

## 4.2 Block 5 — Instrument description

Block 5 is optional for the first version, because an ESP32 controls a single logical instrument.

It is nonetheless recommended in order to make the protocol explicit and to allow future extensions.

### Request

```text
F0 7D 00 05 00 F7
```

### Response for a single instrument

```text
F0 7D 00 05 01
01
01
<channel>
<gm_program>
<type_id>
F7
```

Example for a guitar on internal MIDI channel 1, corresponding to user channel 2:

```text
F0 7D 00 05 01 01 01 01 18 04 F7
```

With:

```text
internal MIDI channel = 1
GM program            = 24, nylon guitar
type                  = 0x04, guitar
```

---

## 4.3 Block 6 — Instrument capabilities

Block 6 is mandatory.

### Request

```text
F0 7D 00 06 00 <channel> F7
```

### Response

```text
F0 7D 00 06 01
<version>
<channel>
<gm_program>
<type_id>
<sub_type>
<notes_mode>
<note_min>
<note_max>
<polyphony>
<discrete_notes_count>
<discrete_notes...>
<cc_count>
<supported_ccs...>
<name_length>
<name...>
F7
```

---

# 5. Automatic computation of the playable range

The note range must not be entered manually when the strings and frets are already configured.

For each string:

```text
minimum note = open-string note + capo

maximum note =
open-string note
+ capo
+ maximum number of frets of the string
```

The controller then builds the union of all playable notes.

## 5.1 Continuous range mode

Range mode may be used when all notes between the minimum note and the maximum note are playable.

```text
notes_mode = 0
```

The block then contains:

```text
note_min
note_max
discrete_notes_count = 0
```

## 5.2 Discrete notes mode

If certain notes located between the limits are not playable on any string:

```text
notes_mode = 1
```

The system must transmit the exact list of playable notes.

This case can occur with:

* particular tunings;
* few frets;
* different fret counts across strings;
* disabled strings;
* forbidden mechanical zones.

---

# 6. Polyphony computation

The announced polyphony must represent the maximum number of notes that can be held simultaneously.

Initial automatic value:

```text
polyphony =
number of active and functional strings
```

It must however be able to be limited in the Web interface.

Examples:

```text
6 strings with individual plectrums → maximum polyphony 6

6 strings, per-string plucking
but independent sustain              → maximum polyphony 6

6 strings with mechanical constraints
limiting play to 4 strings           → polyphony configured to 4
```

The setting must offer:

```text
Automatic
Custom value
```

---

# 7. Announced MIDI controllers

Block 6 must declare the CCs that are actually enabled.

Possible list:

|    CC | Function                       |
| ----: | ------------------------------ |
|   CC7 | master volume                  |
|  CC11 | expression                     |
|  CC20 | default string selection       |
|  CC21 | default fret selection         |
|  CC64 | sustain                        |
| CC120 | immediate sound off            |
| CC123 | all notes off                  |

CC20 and CC21 must be replaced by the numbers actually configured in the interface.

Example:

```text
configured string CC = 24
configured fret CC = 25
```

Block 6 must announce:

```text
7, 11, 24, 25, 64, 120, 123
```

A disabled CC must not be announced.

---

# 8. Block 7 — String configuration

Block 7 is mandatory.

### Request

```text
F0 7D 00 07 00 <channel> F7
```

### General-Midi-Boop version 1 compatible response

```text
F0 7D 00 07 01
01
<channel>
<string_count>
<fret_count>
<is_fretless>
<capo>
<cc_active>
<cc_string>
<cc_fret>
<tuning...>
F7
```

For Stepper-Plucked-Strings-GMB:

```text
is_fretless = 0
```

The tuning must be transmitted in the order defined by General-Midi-Boop:

```text
lowest string
to
highest string
```

---

# 9. Limitations of block 7 version 1

The current version of block 7 transmits:

* a global number of frets;
* the string and fret CC numbers;
* the tuning.

It does not yet transmit:

* a different number of frets per string;
* the minimum and maximum CC values;
* the CC offsets;
* the reversed string order;
* the custom logical-string to physical-string table;
* the automatic, explicit or hybrid mode.

These settings remain stored in the ESP32, but cannot all be transmitted with block 7 version 1.

The first firmware version must remain compatible with block 7 version 1.

A version 2 extension will have to be added jointly in General-Midi-Boop.

---

# 10. Proposed extension of block 7 version 2

## 10.1 Objective

Version 2 must transmit all the configuration needed by the tablature and string/fret selection system.

### Proposed response

```text
F0 7D 00 07 01
02
<channel>
<string_count>
<fret_count_global>
<is_fretless>
<capo>

<cc_active>
<cc_string>
<cc_fret>

<cc_string_min>
<cc_string_max>
<cc_string_offset_encode>

<cc_fret_min>
<cc_fret_max>
<cc_fret_offset_encode>

<selection_mode>
<string_order>

<tuning[num_strings]>
<frets_per_string[num_strings]>
<string_mapping[num_strings]>
F7
```

## 10.2 Selection modes

```text
0 = automatic allocation
1 = explicit selection by CC
2 = hybrid mode
```

## 10.3 String order

```text
0 = normal order
1 = reversed order
2 = custom mapping
```

## 10.4 Signed offsets

Since SysEx data must remain between 0 and 127, a signed offset must be encoded as follows:

```text
transmitted_value = offset + 64
```

Available range:

```text
-64 to +63
```

Decoding:

```text
offset = transmitted_value - 64
```

## 10.5 Compatibility

The firmware must be able to respond in version 1 as long as General-Midi-Boop does not support version 2.

Support for version 2 must be enabled only after its parser is added in General-Midi-Boop.

---

# 11. Capabilities change notification

## 11.1 Problem

The current protocol is initiated by General-Midi-Boop:

```text
General-Midi-Boop sends a request
ESP32 sends a response
```

If the user modifies the configuration from the ESP32 Web interface, General-Midi-Boop does not automatically know that the capabilities have changed.

## 11.2 Proposed solution

Add a notification block:

```text
Block 8 — Capabilities Changed
```

The notification does not carry all the capabilities.

It tells General-Midi-Boop that it must restart the discovery procedure.

### Proposed format

```text
F0 7D 00 08 02
01
<channel>
<revision_configuration[5]>
<flags_modification>
F7
```

### Fields

| Field    | Description                                        |
| -------- | -------------------------------------------------- |
| `08`     | notification block                                 |
| `02`     | spontaneous notification                           |
| `01`     | block version                                      |
| channel  | channel concerned                                  |
| revision | configuration number encoded on 5 bytes of 7 bits  |
| flags    | modified categories                                |

### Modification flags

| Bit | Meaning                              |
| --: | ------------------------------------ |
|   0 | identity modified                    |
|   1 | descriptor modified                  |
|   2 | general capabilities modified        |
|   3 | string configuration modified        |
|   4 | CC mapping modified                  |
|   5 | restart or new homing required       |
|   6 | reserved                             |

---

# 12. Expected reaction of General-Midi-Boop

After receiving block 8:

```text
1. identify the device;
2. read the channel concerned;
3. compare the revision number;
4. send a new Block 1 request;
5. re-read the feature flags;
6. send a Block 6 request;
7. send a Block 7 request;
8. send a Block 5 request if necessary;
9. update the instrument;
10. report the new configuration in the interface.
```

General-Midi-Boop must not replace the configuration until all the expected responses are valid.

In case of failure, the old configuration must remain active with a warning.

---

# 13. Configuration revision number

Each valid save must increment a counter:

```text
capabilitiesRevision
```

Example:

```text
initial configuration : 1
tuning modification    : 2
CC modification        : 3
fret modification      : 4
```

The counter must be:

* saved in persistent memory;
* included in the block 8 notification;
* displayed in the Web interface;
* used to avoid unnecessary refreshes.

A simple restart without modification must not necessarily increment the revision.

---

# 14. Startup behavior

```text
ESP32 startup
      │
      ▼
Loading the active profile
      │
      ▼
Validation
      │
      ├── invalid profile → SAFE mode
      │
      ▼
Building the capabilities snapshot
      │
      ▼
Initializing the Wi-Fi MIDI transport
      │
      ▼
Waiting for SysEx requests
```

When General-Midi-Boop connects:

```text
Block 1 requested
      ↓
identity response
      ↓
Block 6 requested
      ↓
capabilities response
      ↓
Block 7 requested
      ↓
string configuration response
```

---

# 15. Behavior after Web modification

```text
User modifies a parameter
              │
              ▼
Configuration placed in draft
              │
              ▼
Full validation
              │
      ┌───────┴────────┐
      │                │
   invalid            valid
      │                │
 display error          ▼
                  atomic save
                        │
                        ▼
                 revision increment
                        │
                        ▼
                capabilities rebuild
                        │
                        ▼
                 Block 8 notification
```

A configuration being edited must never be announced.

Only the validated and activated profile must be published.

---

# 16. Applying mechanical modifications

Some modifications can be applied immediately:

* name;
* GM program;
* CC numbers;
* selection mode;
* allocation strategy.

Others require returning to a safe state:

* changing the number of strings;
* changing pins;
* changing the steps/mm ratio;
* changing the motor direction;
* changing the limits;
* changing the HOME sensor;
* changing the fret positions.

For these modifications:

```text
1. stop new notes;
2. finish or cancel active notes;
3. disable the actuators;
4. apply the configuration;
5. perform a homing if necessary;
6. rebuild the capabilities;
7. send the SysEx notification.
```

The interface must clearly indicate:

```text
Immediate application
Homing required
Restart required
```

---

# 17. Web interface — Identity and capabilities

A page must be added:

```text
MIDI > GMB Identity and capabilities
```

## 17.1 Simplified mode

The simplified mode must offer:

* enabling General-Midi-Boop detection;
* instrument name;
* instrument type;
* instrument preset;
* General MIDI program;
* MIDI channel;
* "Publish capabilities" button;
* "Test communication" button;
* status of the last detection.

The computed capabilities must be displayed read-only:

```text
Strings              : 4
Frets                : 12
MIDI range           : 40 to 76
Polyphony            : 4
String CC            : 20
Fret CC              : 21
Tuning               : E2 A2 D3 G3
Revision             : 7
```

## 17.2 Advanced mode

The advanced mode must allow:

* enabling blocks 5, 6 and 7;
* selecting the block 7 version;
* overriding the computed polyphony;
* choosing continuous range or discrete notes;
* viewing the announced CCs;
* viewing the SysEx bytes;
* manually sending each response;
* sending the modification notification;
* resetting the identifier;
* exporting the capabilities snapshot.

---

# 18. Built-in SysEx tester

The interface must be able to simulate the following requests:

```text
Request identity
Request descriptor
Request capabilities
Request string configuration
Notify a modification
Perform a full discovery
```

For each test, the interface must display:

* message sent;
* message received;
* field decoding;
* 7-bit validity;
* length;
* possible error;
* response time.

Example:

```text
Request:
F0 7D 00 06 00 00 F7

Response:
F0 7D 00 06 01 ...

Channel     : 1
Type        : guitar
Range       : E2 to E5
Polyphony   : 4
CC          : 7, 11, 20, 21, 64, 120, 123
Result      : valid
```

---

# 19. Capabilities snapshot

The firmware must produce an immutable internal structure:

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
```

When a SysEx response begins, it must use a single snapshot.

A configuration change during the send must not produce a response mixing two versions of the profile.

---

# 20. Protocol safety

The firmware must:

* verify each request length;
* verify the header;
* verify the General-Midi-Boop identifier;
* ignore unknown directions;
* limit all data bytes between 0 and 127;
* limit the maximum size of a message;
* avoid any dynamic memory allocation during processing;
* limit the response rate;
* refuse nonexistent channels;
* never transmit the Wi-Fi password;
* never transmit sensitive data.

Unknown SysEx messages must be ignored without affecting musical operation.

---

# 21. Transport independence

The SysEx handler must be independent of the MIDI transport.

```text
Wi-Fi MIDI ───┐
BLE MIDI ─────┤
USB MIDI ─────┼──► MidiMessageRouter ─► GmbSysExService
MIDI DIN ─────┤
serial ───────┘
```

The first version uses Wi-Fi.

Future Bluetooth and wired versions must reuse exactly:

* the same blocks;
* the same encoder;
* the same decoder;
* the same snapshot;
* the same tests.

The transport must only transmit complete MIDI bytes.

---

# 22. Initial compatibility

## First version

The first version must implement:

```text
Block 1 version 1
Block 5 version 1, recommended
Block 6 version 1
Block 7 version 1
```

This ensures compatibility with the current General-Midi-Boop protocol.

## Next extension

A coordinated evolution of both repositories must add:

```text
Block 7 version 2
Block 8 modification notification
```

As long as block 8 is not supported by General-Midi-Boop, the interface must provide:

```text
Re-announce the capabilities
```

This action may:

* send the SysEx responses spontaneously;
* or trigger a transport reconnection;
* or ask General-Midi-Boop to restart its detection.

The exact method will have to be aligned with the General-Midi-Boop implementation.

---

# 23. Mandatory tests

## Identification

* correct response to block 1;
* stable identifier;
* correctly truncated name;
* correct version;
* correct flags.

## Capabilities

* correctly computed range;
* correctly generated discrete notes;
* correct polyphony;
* configured CCs correctly announced;
* consistent name and type.

## Strings

* correct number of strings;
* correct number of frets;
* tuning in the correct order;
* correct capo;
* default CC20/CC21;
* custom CCs correctly announced.

## Modifications

* name change;
* tuning change;
* CC change;
* fret count change;
* string count change;
* revision increment;
* notification sent;
* new query successful.

## Robustness

* truncated request;
* invalid channel;
* unknown block;
* data greater than 127;
* requests repeated rapidly;
* modification during a query;
* Wi-Fi loss during a response.

---

# 24. Acceptance criteria

The capabilities communication will be considered functional when:

1. the ESP32 responds to the identity request;
2. its identifier remains stable;
3. the announced features match the available blocks;
4. General-Midi-Boop can automatically read the instrument type;
5. the note range is computed from the tuning and the frets;
6. the polyphony is computed or configured;
7. the supported CCs are announced;
8. the number of strings and the tuning are transmitted;
9. the string and fret CCs are transmitted;
10. the responses are generated from the active profile;
11. a draft configuration is never published;
12. any valid modification increments the revision;
13. a modification can trigger a new discovery;
14. the firmware remains compatible with blocks 1, 6 and 7 version 1;
15. the architecture allows adding block 7 version 2;
16. the architecture allows adding the block 8 notification;
17. the SysEx service works independently of the MIDI transport.
