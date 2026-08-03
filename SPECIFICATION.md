# Specification — Stepper-Plucked-Strings-GMB

**Version:** 1.0
**Status:** initial specification
**Target platform:** ESP32-S3
**Number of strings:** 1 to 6
**Initial communication:** Wi-Fi
**Configuration:** local Web interface
**Instrument type:** plucked or strummed string instruments

---

# 1. Project purpose

Stepper-Plucked-Strings-GMB is a modular MIDI controller intended for plucked or strummed string instruments.

The system must move a single mechanical finger along each string in order to select the note to play.

Each string has its own axis:

```text
String 1 → stepper motor 1 → movable finger 1
String 2 → stepper motor 2 → movable finger 2
String 3 → stepper motor 3 → movable finger 3
...
String 6 → stepper motor 6 → movable finger 6
```

For each string:

```text
1 stepper motor
1 linear axis
1 carriage
1 movable finger
1 finger pressing mechanism
1 pluck mechanism
1 reference sensor
```

The stepper motor handles exclusively the longitudinal movement of the finger.

Pressing, plucking and damping may be handled by servomotors or other auxiliary actuators, but they do not replace the stepper motor used to select the note.

---

# 2. Position within the GMB family

The project must remain specialized in order to avoid an overly complex universal firmware.

The note-selection technologies will be split across separate projects:

```text
Stepper-Plucked-Strings-GMB
└── a stepper motor moves a single finger per string

Servo-Plucked-Strings-GMB
└── several fixed servomotors actuate different positions

Solenoid-Plucked-Strings-GMB
└── several fixed solenoids actuate different positions
```

This specification concerns only:

```text
Stepper-Plucked-Strings-GMB
```

A common base may later be extracted for:

* MIDI processing;
* communication;
* Web configuration;
* profile management;
* diagnostics.

The mechanical logic of each project must nonetheless remain independent.

---

# 3. Target instruments

The system must be adaptable to:

* ukulele;
* guitar;
* bass;
* mandolin;
* banjo;
* tenor guitar;
* zither;
* experimental plucked string instruments;
* instruments using an individual pick;
* instruments using a per-string strum.

The project must not impose:

* a specific tuning;
* a fixed number of strings;
* a fixed number of frets;
* a single vibrating length;
* a single transmission model;
* a single type of servomotor;
* a fixed GPIO wiring.

---

# 4. Excluded functions

This version must not handle:

* bowed string instruments;
* linear bows;
* bow wheels;
* DC friction motors;
* BLDC motors;
* bow speed regulation;
* a matrix of fixed servomotor-driven fingers;
* a matrix of fixed solenoid-driven fingers;
* multiple movable fingers on the same string;
* a stepper motor shared between several strings.

---

# 5. Reference mechanical architecture

## 5.1 String channel

Each string constitutes an independent channel.

```text
Stepper motor
        ↓
Mechanical transmission
        ↓
Longitudinal carriage
        ↓
Single finger
        ↓
Position on the string
```

The transmission may use:

* GT2 belt;
* trapezoidal lead screw;
* ball screw;
* rack and pinion;
* cable;
* experimental mechanism.

The firmware must use a physical unit that is independent of the transmission type:

```text
position in millimeters
        ↓
conversion
        ↓
position in motor steps
```

## 5.2 Finger pressing

Once the carriage is positioned, the finger must be able to:

* descend onto the string;
* hold the string;
* lift back up;
* stay raised during movements;
* stay raised for an open string.

The reference mechanism uses one servomotor per string.

## 5.3 Setting the string in vibration

Two modes must be provided.

### Individual pluck

Each string has its own pluck actuator.

```text
1 pluck servo per string
```

This mode allows:

* simultaneous chords;
* repeated notes;
* individual tremolo;
* individual velocity control;
* precise triggering of each string.

### Per-string strum

Each string may use its own strum servo instead of an individual pluck.

It must allow:

* upward strum;
* downward strum;
* adjustable speed;
* return to rest position;
* synchronization with the fingers.

The same instrument may combine strings that pluck with strings that strum.

---

# 6. Target capacity

| Resource                | Minimum |   Maximum |
| ----------------------- | ------: | --------: |
| Strings                 |       1 |         6 |
| Stepper motors          |       1 |         6 |
| Movable fingers         |       1 |         6 |
| Reference sensors       |       1 |         6 |
| Opposite endstops       |       0 |         6 |
| Pressing servos         |       1 |         6 |
| Pluck servos            |       0 |         6 |
| Auxiliary servos        |       0 |         4 |
| Total servo outputs     |       1 |        16 |
| Auxiliary power outputs |       0 |         8 |
| Saved profiles          |       1 | 8 minimum |

The following relationship must be preserved:

```text
number of active strings
=
number of active stepper axes
=
number of movable fingers
```

---

# 7. Electronic architecture

```text
                         Wi-Fi
                           │
               MIDI + Web configuration
                           │
                           ▼
                       ESP32-S3
                           │
        ┌──────────────────┼──────────────────┐
        │                  │                  │
 STEP/DIR command         I²C              Sensors
        │                  │                  │
 1 to 6 drivers         PCA9685          HOME / LIMIT
        │                  │
 1 to 6 motors      1 to 16 servos
```

## 7.1 Main controller

The reference platform must use an ESP32-S3.

The controller must handle:

* reception of MIDI commands over Wi-Fi;
* hosting of the Web interface;
* note allocation;
* trajectory generation;
* state machine management;
* control of the PCA9685;
* sensor monitoring;
* profile storage;
* diagnostics;
* safety.

The ESP32-S3 has a GPIO matrix that allows routing many peripheral signals to different GPIOs. This flexibility makes it possible to use board profiles and a configurable pin assignment.

## 7.2 Stepper drivers

The reference driver must be the TMC2209 or a STEP/DIR-compatible driver.

Each axis must have:

```text
STEP
DIR
ENABLE
HOME
LIMIT optional
DIAG optional
UART optional
```

The first prototype board must accept pluggable driver modules.

This makes the following easier:

* replacing a driver;
* testing with several models;
* maintenance;
* adapting the motor current;
* prototyping before creating an integrated PCB.

## 7.3 Servomotors

A PCA9685 must provide up to 16 servo outputs.

Recommended allocation:

| Channels | Use                                          |
| -------- | -------------------------------------------- |
| 0 to 5   | finger pressing                              |
| 6 to 11  | individual pluck                             |
| 12 to 15 | dampers or auxiliary functions               |

The `OE` output of the PCA9685 must be connected to a safety pin in order to immediately neutralize the servos.

---

# 8. Communication

## 8.1 Initial version: Wi-Fi

The first version must operate exclusively over Wi-Fi for external communications.

Two network modes must be offered.

### Access point mode

The ESP32 creates its own Wi-Fi network.

```text
Default SSID:
Stepper-Plucked-Strings-GMB

Configuration address:
displayed local address or captive portal
```

This mode must allow an initial configuration without a router.

### Wi-Fi client mode

The ESP32 joins the user's local network.

The system must store:

* SSID;
* password;
* instrument network name;
* optional fixed address;
* mDNS name;
* reconnection parameters.

If the connection fails several times, the system must automatically fall back to access point mode.

## 8.2 Initial MIDI transport

The transport layer must be separated from the internal MIDI engine.

Wi-Fi inputs may include:

* binary WebSocket;
* RTP-MIDI;
* configurable UDP protocol;
* test commands from the Web interface.

All transports must produce a common internal event:

```cpp
struct MidiEvent {
    uint32_t timestampUs;
    uint8_t source;
    uint8_t type;
    uint8_t channel;
    uint8_t data1;
    uint8_t data2;
};
```

## 8.3 Future extensions

The architecture must make it possible to add later:

```text
BLE MIDI
USB MIDI
MIDI DIN
serial link
CAN or RS485
```

Adding a new transport must not modify:

* the string controller;
* the note allocator;
* motion management;
* mechanical profiles.

GPIO19 and GPIO20 must remain reserved by default in order to preserve the ability to later use the native USB of the ESP32-S3. These pins are used by the component's native USB-JTAG/USB interface.

---

# 9. Web interface

## 9.1 Objective

The interface must allow a beginner to configure the instrument without modifying the source code.

It must work from:

* computer;
* tablet;
* phone.

No dedicated application must be required.

## 9.2 Two interface levels

### Simplified mode

Intended for beginners.

It must offer:

* step-by-step wizard;
* recommended values;
* automatic pin assignment;
* wiring diagrams;
* test buttons;
* automatic validation;
* understandable error messages.

### Advanced mode

Intended for fine-tuning.

It must allow:

* manual GPIO assignment;
* speed adjustment;
* acceleration adjustment;
* delay adjustment;
* modification of velocity curves;
* access to diagnostics;
* editing of detailed parameters;
* JSON import and export.

---

# 10. First-configuration wizard

The interface must guide the user in the following order.

## Step 1 — Identification

* instrument name;
* optional description;
* number of strings;
* instrument type;
* proposed tuning;
* maximum number of frets.

## Step 2 — Board selection

The user selects:

* ESP32 board model;
* revision;
* Flash capacity;
* presence of PSRAM;
* module variant.

The board profile automatically determines:

* the available GPIOs;
* the reserved GPIOs;
* the recommended GPIOs;
* the GPIOs to use with caution;
* the functions present on the board.

The initial profile must support:

```text
ESP32-S3-DevKitC-1
```

The DevKitC-1 board exposes most of the module's GPIOs, but some variants use GPIO35, GPIO36 and GPIO37 for the internal Flash or PSRAM. These pins must therefore not be offered without checking the selected variant.

## Step 3 — Automatic assignment

The following button must be offered:

```text
Automatically assign the pins
```

The system chooses a conflict-free configuration based on:

* the number of strings;
* the enabled interfaces;
* the selected board;
* future USB usage;
* the need to preserve the diagnostic port;
* the I²C peripherals;
* the number of sensors.

## Step 4 — Mechanical configuration

For each string:

* open MIDI note;
* maximum number of frets;
* vibrating length;
* transmission type;
* motor steps per revolution;
* microstepping;
* travel per revolution;
* direction inversion;
* maximum speed;
* maximum acceleration;
* rest position.

## Step 5 — Homing

For each axis:

* sensor enabled;
* sensor GPIO;
* NO or NC contact;
* active level;
* homing direction;
* fast speed;
* slow speed;
* back-off distance;
* offset after origin;
* timeout;
* maximum search limit.

## Step 6 — Servo calibration

For each servo:

* PCA9685 channel;
* rest position;
* active position;
* minimum and maximum limits;
* inverted direction;
* travel time;
* settling time;
* deactivation at rest.

## Step 7 — Note calibration

Two methods must be offered:

```text
Automatic fret calculation
Manual calibration of each position
```

## Step 8 — Test

The user must be able to test:

* each motor;
* each sensor;
* each finger;
* each pick;
* each note;
* each string;
* a chord;
* the general stop.

## Step 9 — Validation

The interface displays:

```text
Valid configuration
```

or a precise list of the problems.

No actuator must be activated in normal mode as long as critical errors are not corrected.

---

# 11. Configurable GPIO management

## 11.1 Principle

The firmware must not use a single global list identical for all boards.

Each board must have a profile:

```cpp
struct BoardProfile {
    const char* identifier;
    const char* displayName;
    PinCapability pins[MAX_BOARD_PINS];
};
```

Each GPIO must be described by capabilities:

```cpp
struct PinCapability {
    int8_t gpio;
    bool exposed;
    bool input;
    bool output;
    bool interrupt;
    bool highSpeedOutput;
    bool internalPullUp;
    bool internalPullDown;
    bool adc;
    bool reserved;
    bool strapping;
    bool usb;
    bool onboardPeripheral;
    PinPreference preference;
};
```

## 11.2 Displayed categories

In the interface:

```text
Green   → recommended
Yellow  → usable with caution
Red     → reserved or incompatible
Gray    → already in use
```

By default, a beginner must see only the recommended GPIOs.

The yellow GPIOs must be accessible only in advanced mode, with an explanation.

The red GPIOs must not be selectable.

## 11.3 List filtered according to use

When configuring a `STEP` signal, the list must offer only the GPIOs that are:

* capable of operating as an output;
* suitable for fast signals;
* not reserved;
* not assigned;
* compatible with the step generator.

For `HOME` or `LIMIT`, the list must offer only the GPIOs that are:

* capable of operating as an input;
* compatible with interrupts;
* having a suitable bias or using an external resistor;
* not assigned.

For `SDA` and `SCL`, the list must offer:

* the GPIOs usable by I²C;
* a recommended default pair;
* no pin already in use.

For a future USB interface, GPIO19 and GPIO20 must be automatically reserved.

## 11.4 ESP32-S3 restrictions

The pin manager must be aware of at least the following restrictions:

* GPIO0, GPIO3, GPIO45 and GPIO46 are strapping pins;
* GPIO19 and GPIO20 are used by the native USB-JTAG/USB;
* GPIO26 to GPIO32 are normally tied to the Flash or the PSRAM;
* GPIO33 to GPIO37 may also be used by the memory on some variants;
* GPIO48 drives the RGB LED on the DevKitC-1;
* GPIO43 and GPIO44 are tied to the main UART port of the DevKitC-1.

These pins are not necessarily unusable in all cases, but they must be classified according to the exact board profile.

## 11.5 Recommended profile for ESP32-S3-DevKitC-1

Initial example of automatic assignment:

| Function              | Proposed GPIOs         |
| --------------------- | ---------------------- |
| STEP 1 to 6           | 4, 5, 6, 7, 15, 16     |
| DIR 1 to 6            | 17, 18, 8, 9, 10, 11   |
| HOME 1 to 6           | 12, 13, 14, 21, 38, 39 |
| I²C SDA               | 40                     |
| I²C SCL               | 41                     |
| Global ENABLE         | 42                     |
| PCA9685 safety output | 47                     |

This assignment constitutes an initial software profile and not a universal rule.

It must be replaceable from the interface.

Pins kept reserved by default:

| GPIO       | Reservation                           |
| ---------- | ------------------------------------- |
| 19, 20     | future USB                            |
| 43, 44     | programming and diagnostic UART       |
| 0          | boot/BOOT                             |
| 3, 45, 46  | strapping                             |
| 48         | onboard LED                           |
| 35, 36, 37 | dependency on the Flash/PSRAM variant |

## 11.6 Conflict detection

The validator must prevent:

* two signals using the same GPIO;
* an input assigned to an unavailable pin;
* a STEP signal on an incompatible pin;
* the use of a reserved GPIO;
* the use of GPIO19 or GPIO20 when USB is reserved;
* the selection of a Flash/PSRAM pin;
* the simultaneous use of an onboard LED and the same GPIO;
* the unintentional replacement of the diagnostic port.

Each error must explain:

```text
why the pin is incompatible
which pin to choose instead
which function already uses the pin
```

---

# 12. Stepper motor configuration

Each axis must have the following parameters:

```text
axis enabled
GPIO STEP
GPIO DIR
GPIO ENABLE or ENABLE group
GPIO HOME
GPIO LIMIT optional
GPIO DIAG optional
inverted motor direction
inverted ENABLE level
motor steps per revolution
microstepping
mechanical travel per revolution
steps per millimeter
maximum speed
maximum acceleration
fast homing speed
slow homing speed
back-off distance
reference offset
minimum position
maximum position
deactivation delay
indicative motor current
```

## 12.1 Assisted calculation

The interface must automatically compute the steps per millimeter.

### Belt

```text
stepsPerMm =
stepsPerRevolution × microsteps
────────────────────────────
pulleyTeeth × beltPitch
```

### Screw

```text
stepsPerMm =
stepsPerRevolution × microsteps
────────────────────────────
leadPerRevolution
```

The user must be able to select:

```text
GT2 belt
Screw
Custom value
```

---

# 13. Homing

Homing must be non-blocking and independent for each string.

```text
CHECK_SENSOR
      ↓
SEEK_FAST
      ↓
SENSOR_DETECTED
      ↓
BACKOFF
      ↓
SEEK_SLOW
      ↓
SET_ZERO
      ↓
MOVE_TO_OFFSET
      ↓
READY
```

## 13.1 Parallel homing

The motors may perform their homing simultaneously.

An option must allow:

```text
Simultaneous homing
Sequential homing
Homing by groups
```

The sequential mode may be used when the power supply is limited.

## 13.2 Detected faults

* sensor active at startup;
* sensor impossible to release;
* sensor never reached;
* unstable sensor;
* timeout;
* maximum distance exceeded;
* inconsistent activation of HOME and LIMIT.

A faulty axis must be disabled without causing unexpected movement on the other axes.

---

# 14. Note configuration

## 14.1 Tuning

Each string has:

```text
open MIDI note
maximum fret included
position of each fret
```

The system must offer predefined tunings:

* guitar;
* bass;
* ukulele;
* mandolin;
* banjo;
* custom configuration.

The predefined tunings must remain fully modifiable.

## 14.2 Theoretical calculation

The theoretical position of a fret is:

```text
position =
vibrating length × (1 - 2^(-fret / 12))
```

## 14.3 Manual calibration

For each fret, the user must be able to:

1. select the fret;
2. move the motor with buttons;
3. test the note;
4. adjust the position;
5. save the exact position.

The calibrated table must take priority over the theoretical position.

## 14.4 Compensation

The system must allow:

* individual correction of a fret;
* correction according to the direction of movement;
* compensation for mechanical backlash;
* global string offset;
* forward and backward software limit.

---

# 15. Servo configuration

Each servo must use pulses calibrated in microseconds.

Parameters:

```text
servo enabled
PCA9685 channel
function
minimum pulse
maximum pulse
rest position
active position
position A
position B
inverted direction
travel time
settling time
deactivation at rest
```

## 15.1 Finger

The finger servo must have:

```text
raised position
pressed position
delay after press
delay after release
```

## 15.2 Individual pick

The pick must have:

```text
left position
right position
rest position
automatic alternation
minimum travel
maximum travel
movement speed or delay
```

## 15.3 Open string

For an open string:

```text
finger raised
motor possibly moved to a safety position
plucking allowed directly
```

An advanced option may allow using the finger on fret zero for a specific mechanism.

---

# 16. State machines

Each string must use an independent state machine.

```text
DISABLED
HOMING
IDLE
RELEASING_FINGER
MOVING
PRESSING_FINGER
SETTLING
READY_TO_PLUCK
PLUCKING
SUSTAINING
DAMPING
CANCELLING
FAULT
```

No blocking `delay()` must be used during play.

Each command must have an identifier.

If a command is cancelled or replaced, all deferred actions associated with its old identifier must be ignored.

This prevents:

* a pluck after a Note Off;
* a delayed press;
* the execution of an old position;
* an attack after a panic.

---

# 17. Note allocation

## 17.1 Principle

A note must be assigned to a string that is:

* capable of playing the note;
* initialized;
* fault-free;
* available;
* requiring the shortest preparation time.

## 17.2 Chords

Notes received within a configurable window must be grouped.

Initial value:

```text
3 ms
```

The allocator must look for a global assignment.

Order of priorities:

1. play as many notes as possible;
2. respect the mechanical limits;
3. minimize the time before plucking;
4. minimize movements;
5. keep fingers that are already well positioned;
6. limit direction changes.

## 17.3 Saturation strategies

When too many notes are requested:

```text
ignore extra notes
priority to low notes
priority to high notes
priority to the first note received
replace the oldest note
monophonic mode
```

The choice must be accessible in the Web interface.

---

# 18. MIDI parameters

The interface must allow:

* global MIDI channel;
* Omni mode;
* channel per string;
* general transposition;
* per-string transposition;
* note range;
* velocity curve;
* Note Off behavior;
* sustain pedal;
* chord grouping delay;
* saturation strategy.

## 18.1 Velocity

Velocity may act on:

* pick travel;
* pick speed;
* attack delay;
* pluck profile.

Curves offered:

```text
linear
soft
strong
exponential
custom
```

---

# 19. Web dashboard

The main page must display:

```text
general status
Wi-Fi connection
MIDI source
active profile
number of ready strings
notes currently playing
active faults
available temperatures
available voltages
STOP button
```

For each string:

```text
status
current note
current fret
motor position
target position
remaining distance
HOME status
LIMIT status
finger status
pick status
last fault
```

---

# 20. Saving configurations

The system must store at least eight profiles.

Functions:

* create;
* copy;
* rename;
* delete;
* export;
* import;
* restore;
* set the startup profile.

The exchange format must be JSON.

Simplified example:

```json
{
  "project": "Stepper-Plucked-Strings-GMB",
  "profileVersion": 1,
  "instrument": {
    "name": "Ukulele 4 strings",
    "stringCount": 4
  },
  "board": {
    "profile": "esp32-s3-devkitc-1",
    "reserveUsb": true,
    "automaticPinAssignment": true
  },
  "network": {
    "mode": "station",
    "hostname": "gmb-ukulele"
  },
  "strings": []
}
```

The Wi-Fi password must not appear in ordinary exports, except with an explicit option.

---

# 21. Safety

## 21.1 State at startup

At power-on:

```text
drivers disabled
servos neutralized
auxiliary outputs cut
MIDI queues empty
profile checked
GPIOs validated
```

## 21.2 Emergency stop

A hardware stop must be able to:

* disable the drivers;
* disable the PCA9685 via `OE`;
* neutralize the auxiliary outputs;
* keep the ESP32 powered.

## 21.3 Software panic

The panic must:

* flush the MIDI queue;
* cancel all movements;
* cancel all plucks;
* raise the fingers;
* neutralize the servos;
* disable the motors;
* record the cause.

## 21.4 Loss of Wi-Fi

Configurable behavior:

```text
finish active notes then stop
stop immediately
continue commands already queued
return to standby without disabling the motors
```

Default behavior:

```text
cancellation of pending commands
controlled release
return to the READY state
```

---

# 22. Power supply

Recommended rails:

```text
24 V        stepper motors
5 to 7.4 V  servomotors
5 V         logic
3.3 V       ESP32-S3
```

Requirements:

* separate servo power supply;
* motor fuse;
* servo fuse;
* reverse-polarity protection;
* TVS on the motor rail;
* capacitors near the drivers;
* reserve capacitor near the PCA9685;
* structured common ground;
* lockable connectors;
* no servo powered by the ESP32 regulator.

---

# 23. Software architecture

```text
firmware/
├── application/
│   ├── Application
│   ├── Scheduler
│   └── EventBus
├── board/
│   ├── BoardProfile
│   ├── PinManager
│   └── PinValidator
├── communication/
│   ├── WifiManager
│   ├── MidiTransport
│   ├── WebSocketMidi
│   └── FutureTransports
├── midi/
│   ├── MidiParser
│   ├── MidiRouter
│   └── MidiEventQueue
├── instrument/
│   ├── InstrumentController
│   ├── StringController
│   └── NoteAllocator
├── motion/
│   ├── StepperAxis
│   ├── MotionPlanner
│   └── HomingController
├── actuators/
│   ├── ServoManager
│   ├── FingerActuator
│   ├── PluckActuator
│   └── DamperActuator
├── configuration/
│   ├── Profile
│   ├── ProfileValidator
│   └── ProfileStorage
├── safety/
│   ├── SafetyManager
│   └── FaultManager
├── diagnostics/
│   ├── Logger
│   └── DiagnosticService
└── web/
    ├── WebServer
    ├── RestApi
    └── WebSocketStatus
```

---

# 24. Development phases

## Phase 1 — Single-string prototype

* ESP32-S3;
* Wi-Fi;
* minimal Web interface;
* one stepper motor;
* one HOME sensor;
* one finger servo;
* one pluck servo;
* Wi-Fi MIDI test;
* complete state machine;
* panic.

## Phase 2 — Intuitive configuration

* configuration wizard;
* board profile;
* automatic GPIO assignment;
* conflict validation;
* motor calibration;
* servo calibration;
* JSON import/export.

## Phase 3 — Multi-string

* four then six axes;
* PCA9685;
* parallel homing;
* note allocation;
* chords;
* per-string diagnostics.

## Phase 4 — Advanced play

* tremolo;
* damping;
* sustain pedal;
* velocity curves;
* saturation strategies.

## Phase 5 — Dedicated hardware

* electronic schematic;
* PCB;
* protections;
* connectors;
* hardware stop;
* electrical validation;
* wiring documentation.

## Phase 6 — Future communications

* BLE MIDI;
* USB MIDI;
* MIDI DIN;
* additional wired links.

---

# 25. Acceptance criteria

The project will be considered functional when:

1. one to six strings can be configured;
2. each string uses a stepper motor and a single movable finger;
3. the GPIOs can be assigned automatically;
4. the interface offers only GPIOs compatible with the function;
5. pin conflicts are blocked;
6. a beginner can complete the configuration with the wizard;
7. the system works in access point mode without a router;
8. the system can join an existing Wi-Fi network;
9. MIDI commands are received over Wi-Fi;
10. the axes perform reliable homing;
11. open strings are played without finger pressing;
12. a Note Off cancels an attack being prepared;
13. no delayed pluck is executed after a cancellation;
14. six axes can be controlled simultaneously;
15. profiles can be saved, exported and restored;
16. the panic neutralizes all actuators;
17. loss of Wi-Fi produces a controlled stop;
18. the architecture allows the future addition of BLE MIDI and wired MIDI.

---

# 26. Deliverables

The project must provide:

```text
ESP32-S3 firmware
Web interface
profile format
ESP32 board profiles
GPIO assignment manager
electronic schematic
PCB
bill of materials
wiring documentation
first-configuration guide
calibration procedure
test procedure
Wi-Fi MIDI protocol documentation
automated tests
example instrument profiles
```

---

# 27. Recommended repository organization

```text
Stepper-Plucked-Strings-GMB/
├── firmware/
├── web-interface/
├── hardware/
│   ├── schematics/
│   ├── pcb/
│   └── wiring/
├── board-profiles/
├── instrument-profiles/
├── mechanics/
├── tests/
├── docs/
│   ├── SPEC_INDEX.md
│   ├── ARCHITECTURE.md
│   ├── PIN_CONFIGURATION.md
│   ├── WEB_INTERFACE.md
│   ├── MIDI_PROTOCOL.md
│   ├── CALIBRATION.md
│   └── SAFETY.md
└── README.md
```

---

# 28. Initial decisions adopted

```text
Name: Stepper-Plucked-Strings-GMB

Project developed from scratch

Plucked or strummed string instruments only

1 to 6 strings

1 stepper motor per string

1 single movable finger per string

ESP32-S3

TMC2209 or compatible STEP/DIR driver

PCA9685 for the servos

Wi-Fi in the first version

Mandatory local Web interface

Configuration accessible to beginners

Automatic or manual GPIO assignment

Board profiles with pin filtering

GPIO19 and GPIO20 reserved for future USB

BLE MIDI and wired communications added later
```
