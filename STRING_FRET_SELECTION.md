# Explicit string and fret selection via MIDI CC

## 1. Objective

Stepper-Plucked-Strings-GMB must be able to receive an explicit indication of the string and fret to use before a note is triggered.

This function allows the main control system, in particular General-Midi-Boop, to directly transmit a tablature position:

```text
string selection
        ↓
fret selection
        ↓
Note On
```

Default convention:

```text
CC20 = string number
CC21 = fret number
```

Example:

```text
CC20 value 3
CC21 value 5
Note On 60 velocity 100
```

Interpretation:

```text
play MIDI note 60
on physical string 3
at fret 5
with a velocity of 100
```

The controller numbers and values must not be hard-coded. All parameters must be editable from the Web interface.

---

## 2. Selection modes

The interface must offer three modes.

### 2.1 Automatic allocation

```text
Mode: Automatic
```

The controller receives only the `Note On`.

It chooses by itself:

* the string capable of playing the note;
* the corresponding fret;
* the string requiring the least movement;
* the combination best suited for a chord.

This mode remains compatible with standard MIDI files that contain no tablature CC.

### 2.2 Explicit selection via CC

```text
Mode: String and fret imposed by MIDI CC
```

The controller uses the CCs received before the `Note On`.

The string and fret are imposed by the MIDI sender.

This mode is intended primarily for General-Midi-Boop and for MIDI files that contain tablature information.

### 2.3 Hybrid mode

```text
Mode: CC priority with automatic fallback allocation
```

Behavior:

1. use the CC selection when it is complete and valid;
2. verify that the string and fret can play the note;
3. use automatic allocation if no valid selection is available.

This mode must be selected by default, because it allows reading:

* standard MIDI files;
* enriched MIDI files;
* commands coming from General-Midi-Boop;
* notes played from the Web interface.

---

## 3. General-Midi-Boop preset

The interface must offer a button:

```text
Use the General-Midi-Boop preset
```

This button automatically applies:

| Parameter                        |          Value |
| -------------------------------- | -------------: |
| Explicit selection enabled       |            yes |
| String selection CC              |             20 |
| Fret selection CC                |             21 |
| First string                     |        value 1 |
| First fret                       |        value 0 |
| String offset                    |              0 |
| Fret offset                      |              0 |
| Consumption mode                 |      next note |
| Selection per MIDI channel       |            yes |
| Fallback allocation              |      automatic |
| Preparation upon CC reception    |            yes |

The maximum values must be adapted automatically to the active profile:

```text
CC string minimum = 1
CC string maximum = number of strings

CC fret minimum = 0
CC fret maximum = instrument's maximum fret
```

For an instrument with four strings and twelve frets:

```text
CC20 : valid values 1 to 4
CC21 : valid values 0 to 12
```

---

## 4. Parameters configurable from the Web interface

The MIDI page must contain a section:

```text
String and fret selection
```

### 4.1 General settings

* enabling explicit selection;
* automatic, explicit, or hybrid mode;
* global MIDI channel;
* independent selection per MIDI channel;
* maximum delay between the CCs and the `Note On`;
* behavior in case of incomplete selection;
* behavior in case of an invalid value;
* validation mode between the note, the string, and the fret.

### 4.2 String selection settings

```text
MIDI controller used
Minimum received value
Maximum received value
Offset
Numbering starting from 0 or 1
Normal or reversed order
Custom mapping table
```

Default values:

```text
CC             = 20
minimum        = 1
maximum        = number of strings
offset         = 0
numbering      = from 1
order          = normal
```

### 4.3 Fret selection settings

```text
MIDI controller used
Minimum received value
Maximum received value
Offset
Value rounding
Handling of out-of-range frets
Custom mapping table
```

Default values:

```text
CC             = 21
minimum        = 0
maximum        = maximum fret
offset         = 0
```

### 4.4 Validity delay

The selection must not remain active indefinitely.

Parameter:

```text
Selection validity duration
```

Proposed value:

```text
100 ms by default
adjustable range : 5 to 2000 ms
```

If no matching `Note On` is received within this delay, the selection must be removed.

---

## 5. Transformation of received values

### 5.1 String

General calculation:

```text
logical string =
received CC value
+ offset
```

The value must then be:

* validated;
* clamped to the allowed range;
* converted to the internal index;
* associated with a stepper axis.

Example with numbering starting from 1:

```text
CC20 = 1 → internal string 0
CC20 = 2 → internal string 1
CC20 = 3 → internal string 2
CC20 = 4 → internal string 3
```

### 5.2 Fret

General calculation:

```text
logical fret =
received CC value
+ offset
```

Example:

```text
CC21 = 0 → open string
CC21 = 1 → first fret
CC21 = 12 → twelfth fret
```

Fret zero must automatically result in:

```text
finger raised
no press on the string
pluck of the open string
```

---

## 6. Reversing the string order

The physical order of the strings may differ from the order used in a tablature or in General-Midi-Boop.

The interface must offer:

```text
Normal order
Reversed order
Custom mapping
```

Example for an instrument with four strings:

### Normal order

| CC value | Physical string |
| -------: | --------------: |
|        1 |               1 |
|        2 |               2 |
|        3 |               3 |
|        4 |               4 |

### Reversed order

| CC value | Physical string |
| -------: | --------------: |
|        1 |               4 |
|        2 |               3 |
|        3 |               2 |
|        4 |               1 |

### Custom mapping

The user must be able to manually select:

```text
CC value 1 → axis 3
CC value 2 → axis 1
CC value 3 → axis 4
CC value 4 → axis 2
```

A visual diagram must display the correspondence between:

* MIDI number;
* musical string;
* physical string;
* stepper axis;
* open-string note.

---

## 7. MIDI reception machine

The controller must store selections as temporary commands.

```cpp
struct PendingStringSelection {
    uint8_t midiChannel;

    bool hasString;
    bool hasFret;

    uint8_t stringValue;
    uint8_t fretValue;

    uint32_t receivedAtUs;
    uint32_t expiresAtUs;
};
```

A complete selection contains:

```text
MIDI channel
string
fret
timestamp
validation state
```

---

## 8. Reliable chord handling

Several simultaneous notes can generate several string/fret selections on the same MIDI channel.

The firmware must therefore not keep only:

```text
last string received
last fret received
```

This method would not be reliable for chords.

The system must use a FIFO queue of selections.

Example of received events:

```text
CC20 string 1
CC20 string 3
CC20 string 4
CC21 fret 2
CC21 fret 5
CC21 fret 7
Note On 42
Note On 55
Note On 64
```

The system must reconstruct:

```text
selection 1 = string 1, fret 2
selection 2 = string 3, fret 5
selection 3 = string 4, fret 7
```

Then associate the `Note On` messages in the same order:

```text
Note 42 → selection 1
Note 55 → selection 2
Note 64 → selection 3
```

The queue must be able to hold at least:

```text
16 pending selections
```

Recommended value:

```text
32 selections
```

---

## 9. Association algorithm

### Reception of the string CC

```text
1. read the value ;
2. apply the offset ;
3. convert to the physical string ;
4. check the range ;
5. create a new pending selection ;
6. record the string.
```

### Reception of the fret CC

```text
1. read the value ;
2. apply the offset ;
3. check the range ;
4. find the oldest selection without a fret ;
5. add the fret to this selection ;
6. mark the selection as complete.
```

### Reception of the Note On

```text
1. find the oldest complete selection on the channel ;
2. associate the note with this selection ;
3. validate the note/string/fret consistency ;
4. remove the selection from the queue ;
5. prepare the motor ;
6. schedule the press and the pluck.
```

This method remains functional if the events of a chord are grouped by type.

---

## 10. Anticipated preparation

As soon as a complete string/fret pair is received, the controller must be able to begin the mechanical preparation, without waiting for the `Note On`.

Sequence:

```text
string CC received
        ↓
fret CC received
        ↓
complete selection
        ↓
finger release
        ↓
motor movement
        ↓
Note On received
        ↓
press and pluck when the position is ready
```

This behavior must be configurable:

```text
Prepare the motor upon reception of the selection
```

Default value:

```text
enabled
```

The `Note On` keeps its role as the musical trigger.

If the motor has not yet reached the fret at the time of the `Note On`:

* the pluck must be put on hold;
* the stepper motor must finish its movement;
* the finger must be pressed;
* the pluck must then be executed;
* no anticipated pluck must be produced.

---

## 11. Consistency between note, string, and fret

For a standard fretted string:

```text
expected note =
open-string note
+ fret number
+ possible transposition
```

The controller must compare:

* the received MIDI note;
* the open-string note of the string;
* the selected fret;
* the capo;
* the configured transposition.

The interface must offer three behaviors.

### Priority to the CCs

```text
The string and fret are used.
The difference with the Note On is only reported.
```

### Priority to the note

```text
The fret is recalculated from the MIDI note.
```

### Strict mode

```text
The note is rejected if the information is inconsistent.
```

Recommended default value:

```text
CC priority with warning
```

General-Midi-Boop must remain able to impose a precise tablature.

---

## 12. Note Off handling

The controller must record the actual assignment of each `Note On`.

```cpp
struct ActiveNote {
    uint8_t midiChannel;
    uint8_t midiNote;
    uint8_t stringIndex;
    uint8_t fret;
    uint32_t noteInstanceId;
};
```

The `Note Off` must not use the last CC value received.

It must retrieve the assignment recorded at the time of the `Note On`.

This allows:

* releasing the correct string;
* handling several strings simultaneously;
* handling chords;
* preventing a new selection from modifying an already active note.

For repeated notes of the same pitch, a stack of instances must be used.

---

## 13. Invalid values

The interface must allow selecting the following behavior:

```text
Reject the command
Clamp to the allowed range
Use automatic allocation
Use the last valid value
```

Default value:

```text
Use automatic allocation and log a warning
```

Examples of invalid values:

* nonexistent string;
* fret exceeding the string's capacity;
* disabled axis;
* faulted string;
* uncalibrated fret;
* expired selection;
* incomplete CC pair.

---

## 14. Simplified configuration for beginners

The simplified screen must display only:

```text
[✓] Enable string/fret selection

System used :
[ General-Midi-Boop ]

String CC :
[ 20 ]

Fret CC :
[ 21 ]

String numbering :
[ 1 to 6 ]

String order :
[ Normal ]

If a CC is missing :
[ Choose automatically ]
```

Available buttons:

```text
Apply the preset
Test reception
Send a test
View received values
```

The advanced parameters must remain hidden in a section:

```text
Advanced settings
```

---

## 15. Web MIDI monitor

The interface must display in real time:

| Time  | Channel | Message    | Value | Interpretation    |
| ----: | ------: | ---------- | ----: | ----------------- |
|  0 ms |       1 | CC20       |     3 | string 3          |
|  1 ms |       1 | CC21       |     5 | fret 5            |
|  2 ms |       1 | Note On 60 |   100 | string 3, fret 5  |

The monitor must also display:

* complete selection;
* pending selection;
* expired selection;
* invalid value;
* automatic allocation used;
* inconsistency between note and fret;
* physical string actually selected.

A button must allow clearing the log.

---

## 16. Integrated test tool

The interface must offer a panel allowing selection of:

```text
String
Fret
MIDI note
Velocity
MIDI channel
```

Then automatically sending:

```text
String CC
Fret CC
Note On
Note Off after a chosen duration
```

The test must display each step:

```text
string CC received
fret CC received
selection validated
axis moving
position reached
finger pressed
string plucked
```

---

## 17. JSON profile parameters

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

    "string": {
      "ccNumber": 20,
      "minimum": 1,
      "maximum": 6,
      "offset": 0,
      "numbering": "oneBased",
      "reverseOrder": false,
      "mapping": [0, 1, 2, 3, 4, 5]
    },

    "fret": {
      "ccNumber": 21,
      "minimum": 0,
      "maximum": 24,
      "offset": 0,
      "invalidValuePolicy": "automaticFallback"
    },

    "validation": {
      "notePositionPolicy": "ccPriorityWithWarning",
      "missingSelectionPolicy": "automaticAllocation",
      "expiredSelectionPolicy": "automaticAllocation"
    }
  }
}
```

---

## 18. Configuration validation

The system must verify:

* string CC between 0 and 119;
* fret CC between 0 and 119;
* two different CC numbers;
* absence of conflict with another configured function;
* string range compatible with the number of strings;
* fret range compatible with the profile;
* complete correspondence between values and axes;
* sufficient queue depth;
* nonzero validity delay.

CC120 to CC127 must not be offered, because they correspond to the MIDI channel mode messages.

The interface may allow standard CCs in advanced mode, but must display a warning in case of potential conflict.

CC20 and CC21 must be presented as the recommended choices.

---

## 19. Acceptance criteria

The string/fret selection will be considered functional when:

1. CC20 selects a string by default;
2. CC21 selects a fret by default;
3. the CC numbers are editable;
4. the ranges and offsets are editable;
5. the string order can be reversed;
6. a custom mapping table can be defined;
7. the automatic mode remains available;
8. the hybrid mode uses the CCs then a fallback allocation;
9. selections are separated by MIDI channel;
10. expired selections are removed;
11. several simultaneous selections can be queued;
12. chords do not depend solely on the last received value;
13. the movement can begin as soon as the CC pair is received;
14. the `Note On` is associated with the correct string and fret;
15. the `Note Off` releases the string actually used;
16. the interface displays the received events and their interpretation;
17. the General-Midi-Boop preset can be applied in one click;
18. an incorrect configuration is detected before the instrument is activated.
