# Safety — Stepper-Plucked-Strings-GMB

> Sources: `SPECIFICATION.md` §21, §22 · Code: `core/safety/SafetyManager.{h,cpp}`.
> Related documents: [`ARCHITECTURE.md`](ARCHITECTURE.md) · [`CALIBRATION.md`](CALIBRATION.md) · [`WEB_INTERFACE.md`](WEB_INTERFACE.md).

The `SafetyManager` centralizes safe states, software panic, hardware emergency
stop, and the fault log.

```cpp
enum class SafetyState {
    ConfigSafe,    // no valid profile — actuators locked out, awaiting configuration
    PowerOnSafe,   // drivers off, servos neutralised, queues empty
    Homing,        // profile validated, outputs live, axes seeking their reference
    Armed,         // normal operation
    Panic,         // latched software panic
    EmergencyStop, // hardware stop asserted
};
```

`actuatorsAllowed()` only returns `true` in the `Armed` state: no actuator moves
in any other state — `ConfigSafe`, `PowerOnSafe` and `Homing` all keep the note
engine **and** every manual mechanical test locked out.

---

## 1. State at startup (§21.1)

### Boot-safe: no fabricated profile (audit P0.1)

With **no valid stored profile** the firmware does **not** invent one. It holds
an empty `Profile` (no axes, no servos, no actuator pins) and latches
`ConfigSafe`: the network and web interface come up so a profile can be built or
loaded, but no actuator can ever be driven and no MIDI reaches the mechanics.

This matters because the alternative is worse than it looks: a synthesised
"default ukulele" profile arms *someone else's* pin map at *someone else's*
speeds on a real machine. The ukulele template still exists in the web UI — it is
simply never applied behind the user's back.

At power-on with a valid profile, `boot()` places the system into `PowerOnSafe`:

```text
drivers disabled
servos neutralised
auxiliary outputs cut
MIDI queues empty
profile verified
GPIO validated
```

The transition to `Armed` is only possible **after** validation of the profile
and the GPIO pins, and only through the `Homing` phase:

```cpp
// PowerOnSafe -> Homing. releaseWaitMs is the time every finger servo needs to
// lift clear of its string before a carriage may move (§16: never drag a finger).
bool beginHoming(bool profileValid, bool pinsValid, uint32_t releaseWaitMs, uint32_t nowMs);
bool releaseComplete(uint32_t nowMs);   // the seek may start
bool armAfterHoming();                  // Homing -> Armed, once every axis is anchored
```

Homing is **sensor-driven**, not a timer, so the final transition is commanded by
the caller once every enabled axis has found HOME. The one timed sub-step lives
in the state machine: the pre-seek finger release.

No actuator is enabled in normal mode as long as critical errors remain
uncorrected (see the wizard §9, [`WEB_INTERFACE.md`](WEB_INTERFACE.md)).

### Complete startup sequence (§13 / §21.1)

`main.cpp` follows a three-phase state machine; **playback is only armed after a
successful homing**, so that no axis moves from an unknown physical position:

```text
ConfigSafe : no valid profile — web/network up, actuators locked out for good
             (a profile must be loaded, which goes through reset())
   │
Boot       : PowerOnSafe — profile loaded and validated, drivers OFF, servos off
   │          (if the profile is invalid, it stays here: no movement)
   ▼
Homing     : PCA channels cleared, THEN /OE enabled, THEN every servo walked to
   │          rest through a GOVERNED park (so arming's in-rush is no worse than
   │          normal play). A refused rest command ABORTS homing — a finger that
   │          may still be pressed must never see a carriage seek.
   │          Once the fingers are up: drivers ON, each axis runs its
   │          HomingController (non-blocking, in parallel), origin anchored on
   │          the HOME sensor (0 mm). A faulty axis is disabled without blocking
   │          the others; with zero working axes the machine hard-stops and
   │          stays out of Ready.
   ▼
Ready      : armAfterHoming() → MIDI notes are played.
```

During `ConfigSafe`, `Boot` and `Homing`, `Note On` messages are not played
(only SysEx requests are processed). A mechanical configuration change from the Web
interface triggers a new homing before playback resumes.

### Hardware emergency stop and limit switches

* **Hardware E-stop**: if an `ESTOP` pin is assigned, `loop()` reads it on every
  pass and immediately triggers a panic (drivers cut off, servos neutralized).
  Without an assigned `ESTOP` pin, only the software panic (Web STOP button /
  CC120/CC123) is available.

  Two wirings are supported, declared per profile by `estopNormallyClosed`:

  | Wiring | Healthy | Asserted |
  | ------ | ------- | -------- |
  | normally **open** (a plain button to GND) | HIGH (pull-up) | LOW |
  | normally **closed** (recommended, `hardware/POWER_AND_SAFETY.md`) | LOW (closed loop to GND) | HIGH — pressed, cut wire, or unplugged connector |

  The normally-closed chain is the one to build: losing the chain reads as
  *asserted*, so a broken wire fails safe instead of silently disarming the
  E-stop.

  **Every** read normalises through `estopAssertedFor()`
  (`core/safety/EstopPolarity.h`) — the boot check, `beginHoming()`, `doReset()`
  and the continuous supervision. That is not decoration: two of those call sites
  used to carry their own `digitalRead(pin) == LOW` test, which is inverted on a
  closed loop. A healthy E-stop looked pressed (so the instrument could never
  home) while a genuinely pressed one went undetected at pre-arm. The predicate
  is a pure constexpr in the core precisely so the truth table can be, and is,
  unit-tested.
* **`LIMIT` switches**: an active `LIMIT` during a movement causes an
  **immediate stop** of the axis concerned (not a deceleration), invalidates its
  position (re-homing required) and puts it into a fault state, without
  disturbing the other axes.

### Recovery after panic / E-stop (`POST /api/reset`)

After a panic or an E-stop, the safety state is **locked**: neither loading a
profile nor a new homing can re-enable the motors. Recovery is explicit via
`POST /api/reset` (the "Reset & re-home" button on the Instrument page), accepted only
if:

* the E-stop is physically released;
* no `LIMIT` is active;
* the profile is valid;
* all axes/servos were able to attach their hardware channel.

Recovery then forces a **new homing** before playback resumes.

### Degraded mode (`readyDegraded`)

If one or more axes fail their homing, the system still enters playback **but**:

* the failing strings are disabled (no note, even under CC selection);
* the **polyphony announced** via SysEx is reduced to the number of functional
  axes and the **capabilities revision** is incremented (General-Midi-Boop stops
  sending the unplayable notes);
* the exposed state becomes `readyDegraded`, the affected lane is flagged on the
  Instrument page, and the fault is logged.

### Wi-Fi secrets and access

* Wi-Fi passwords (station and access point) are stored in **NVS**
  (`Preferences`), never in the exportable profile, and are set via
  `POST /api/wifi`. The access point can be protected with WPA2 (password ≥ 8
  characters); otherwise it remains open.
### Web API authentication

The routes that **move the mechanics or change the configuration**
(`PUT /api/profile`, `/api/profiles*`, `/api/reset`, `/api/test/note`,
`/api/test/servo`, `/api/wifi`) are protected by an **administrator token**
stored in NVS:

* as long as no token is defined (first startup), writes are allowed to enable
  the initial configuration;
* once defined via `POST /api/auth`, each write must provide the matching
  `X-GMB-Token` header; otherwise the request is refused (401);
* `POST /api/panic` remains **always** accessible (safety);
* the status exposes `authConfigured` and `apOpen` so the interface can warn
  when the access point is open **and** without a token.

**Recommendation**: on an untrusted network, set an AP password (WPA2, ≥ 8
characters) **and** an administrator token.

**Known remaining limitations**: no dedicated CSRF/origin protection yet, no
local physical confirmation for reset/homing, and no formal separation between
the MIDI network and the administration network — see the limitations note in
the [`README`](../README.md).

---

## 2. Hardware emergency stop — the PCA9685 /OE (§21.2)

A **hardware** stop must be able to:

* disable the stepper drivers;
* disable the PCA9685 via its `/OE` pin (immediate neutralization of all servos,
  independently of the firmware);
* neutralize the auxiliary outputs;
* **keep the ESP32 powered** (for logging and controlled recovery).

The PCA9685 `/OE` output is wired to a safety pin (GPIO47 in the recommended
DevKitC-1 profile — see [`PIN_CONFIGURATION.md`](PIN_CONFIGURATION.md)).
`emergencyStop(nowMs)` locks the `EmergencyStop` state and records the cause.

---

## 3. Software panic (§21.3)

`panic(cause, nowMs)` locks the `Panic` state, records the cause, and the
firmware must:

* flush the MIDI queue;
* cancel all movements;
* cancel all plucks;
* lift the fingers;
* neutralize the servos;
* disable the motors;
* record the cause.

The `StringController` command-identifier mechanism guarantees that no deferred
attack is executed after a panic (see [`ARCHITECTURE.md`](ARCHITECTURE.md)
§3 and `SPECIFICATION.md` §16). `reset()` returns to `PowerOnSafe` and requires
a re-arm.

The Web API exposes `POST /api/panic` ([`WEB_INTERFACE.md`](WEB_INTERFACE.md)).

---

## 4. Wi-Fi loss (§21.4)

The policy is **fixed and deliberate**: on link loss the firmware cancels pending
commands and releases the sounding notes, but **stays armed** — the instrument
keeps its reference and its readiness, because losing the network is not a
mechanical emergency.

> A `WifiLossBehavior` enum (`FinishThenStop` / `StopImmediately` /
> `ContinueQueued` / `IdleKeepMotors`) used to be documented here (audit P1.10).
> It was **never wired to anything**: no config field selected it and the runtime
> always applied the single policy above, so it advertised a configurability that
> did not exist. It has been removed rather than left as a decorative option. A
> real, selectable policy will return with the `DeviceConfig`/`SafetyConfig`
> split — wired to the runtime at the same time, not before.

---

---

## 4b. hardStop vs controlledPark (audit P0.3)

Two distinct stops, and conflating them is a safety bug:

| | `hardStop` | controlled park |
| --- | --- | --- |
| **When** | E-stop, panic, unrecoverable fault | profile change, normal stop |
| **Servos** | `/OE` off and direct PWM off **at once**; PCA channels cleared only afterwards, so a wedged I2C bus cannot delay the neutralisation | driven to rest with the outputs still live, then the caller waits `parkDurationMs()` before cutting power |
| **Axes** | `stopAll()` (force-stop where they stand) then ENABLE dropped — no deceleration ramp | `controlledStopAll()` (decelerated), drivers left enabled for `stopDurationMs()` |
| **Precondition** | none, ever — a movement is never a precondition for stopping | the rest commands must all be ACCEPTED; a refused one aborts the operation |

A mechanical movement used to precede the stop on the old `neutraliseAll()` path.
It does not any more: a real E-stop cuts first and tidies up afterwards.

---

## 5. Fault log

`SafetyManager` incorporates the role of `FaultManager` (§23):

```cpp
struct FaultRecord { std::string source; std::string message; uint32_t atMs; };

void recordFault(source, msg, nowMs);
const std::vector<FaultRecord>& faults() const;
void clearFaults();
```

Faults are displayed live on the Instrument page, and the cumulative total is
reported by `GET /api/diagnostics` (§19, [`WEB_INTERFACE.md`](WEB_INTERFACE.md) §5.2).

---

## 6. Power supply (§22)

Recommended rails:

| Rail | Usage |
| ---- | ----- |
| 24 V | stepper motors |
| 5 to 7.4 V | servomotors |
| 5 V | logic |
| 3.3 V | ESP32-S3 |

Requirements:

* **separate** servo power supply;
* motor fuse; servo fuse;
* reverse-polarity protection;
* TVS on the motor rail;
* capacitors near the drivers; a reserve capacitor near the PCA9685;
* structured common ground;
* lockable connectors;
* **no servo powered from the ESP32 regulator**.
