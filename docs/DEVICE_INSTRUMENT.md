# Device vs Instrument separation (audit P1.13)

A profile historically mixed two concerns:

- **Device config** — which ESP32 board, the Wi-Fi/network settings, the system
  GPIO pin map, the E-stop contact wiring and the declared power/safety hardware.
  This belongs to the *physical machine* and does not change when you play a
  different tune.
- **Instrument profile** — instrument identity, MIDI mapping, string/fret selector,
  power governor, plucking gesture, the axes (transmission, travel, fret geometry),
  their **homing configuration**, and the servos. This is *portable*: the same
  instrument definition should load onto any compatible device.

  Homing sits on the instrument side deliberately. A homing config describes the
  *carriages* — seek direction and speeds, back-off, the rest offset from the HOME
  sensor, and each endstop's polarity — not the controller board. Carrying an
  instrument to another ESP32 has to bring those references with it, or the first
  re-home on the new device would seek against the wrong reference.

The goal of P1.13 is to make that boundary explicit so the two can evolve — and
eventually be stored and swapped — independently.

## What is in place

### 1. The two structs + a lossless split/merge (host-tested)

`core/configuration/DeviceInstrument.h` defines `DeviceConfig` and `InstrumentProfile`
and the pure functions `deviceConfigOf()`, `instrumentProfileOf()` and
`mergeProfile()`. These split a combined `Profile` into its two halves and recombine
them without loss. Covered by `firmware/test/test_device_instrument.cpp`.

### 2. The split is persisted on disk (this step)

Stored profile **slots** now use a split on-disk layout, while the **interchange
format** used by the web API and by import/export stays flat and unchanged. Two thin
re-parenting wrappers in `ProfileStorage` bridge them, so there is exactly one set of
field (de)serialisers — no duplicated logic:

| Path | Format | Serialiser |
| ---- | ------ | ---------- |
| Web `GET/PUT /api/profile`, import/export | **flat** (unchanged) | `toJson` / `fromJson` |
| On-disk slot files (LittleFS) | **split** | `toSlotJson` / `fromSlotJson` |

On-disk slot shape:

```json
{
  "storageFormat": "gmb-split-v1",
  "project": "...",
  "profileVersion": 2,
  "capabilitiesRevision": 7,
  "device":     { "board": {...}, "pins": [...], "network": {...} },
  "instrument": { "info": {...}, "midi": {...}, "stringFretSelection": {...},
                  "power": {...}, "pluck": {...}, "strings": [...], "servos": [...] }
}
```

`storageFormat` marks the on-disk *layout* generation. It is deliberately **orthogonal
to `profileVersion`**, which continues to version the field *schema* shared with the
flat interchange format. Keeping them separate lets the storage layout change without
disturbing the web contract (the web keeps consuming the flat form at its own version).

### Migration — no stored profile is orphaned

`fromSlotJson` reads a split slot *or* a legacy flat slot (one written before the
split, including an old v1 flat slot, which it migrates via the existing `migrate()`
step). A legacy slot is therefore loaded unchanged and **rewritten in the split form on
the next save** — a lazy, non-destructive migration. The atomic temp-file + `.bak`
save/restore path is unchanged, so a power loss mid-migration never loses a profile.

Covered by `firmware/test/profilecheck` ("device/instrument split slot storage"):
the split shape is asserted, a profile round-trips losslessly through it, and both a
legacy flat slot and a legacy v1 flat slot still load.

## What is deferred (not yet done — honest status)

- ~~**Behavioural portability.**~~ **Done.** The distinction is not *which fields*
  but *which operation*:

  | Operation | What it means | What is taken |
  | --------- | ------------- | ------------- |
  | `PUT /api/profile` | publish the draft you just edited **for this machine** | the whole profile — pins, board and network included, because you meant them |
  | `POST /api/profiles/load` | load a stored **instrument** onto this machine | the instrument half only; the device half of the RUNNING config survives |

  `onActivateProfile(profile, keepDeviceConfig)` carries the distinction, and the
  merge uses `mergeProfile(deviceConfigOf(running), instrumentProfileOf(loaded))`.
  The MERGED profile is what gets validated, so the instrument has to fit *this*
  device's pins — which is the combination that will actually run.

  This was not cosmetic. Adopting a slot's device half replaced the running
  machine's network settings (the radio was not re-initialised, so `/api/status`
  then reported a network the device was not on), its pin map, the declared power
  hardware, and — worst — `estopNormallyClosed`, the E-stop polarity. Importing a
  safety wiring declaration from a file saved on another machine is exactly the
  kind of thing that must not happen quietly.
- **Web/interchange split.** The web UI consumes the flat profile shape at ~100 call
  sites. Restructuring the interchange format would ripple through the browser UI,
  which cannot be functionally validated in the software-only phase — so it is left for
  the bench/browser phase. The on-disk split above needs none of that.
- **`MidiTransportConfig` / `SafetyConfig`** will join `DeviceConfig` as those features
  gain a persisted shape (P1.7 / P1.10 groundwork).

> As with every mechanical item in this repository, only the software behaviour above
> is validated (host tests + sanitizers + ESP32 CI build). Nothing here has been
> exercised on a powered device.
