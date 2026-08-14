# Board profiles

Machine-readable GPIO capability maps for the ESP32 boards supported by
**Stepper-Plucked-Strings-GMB**. The web configurator and the firmware pin
manager use these files to filter which GPIO may carry which signal, per board
and per module variant (SPECIFICATION.md §11).

## These files are GENERATED — do not hand-edit them

The single source of truth is `firmware/src/core/board/BoardProfile.cpp`. These
JSON files are rendered from it by
[`firmware/tools/dump_board_profiles.cpp`](../firmware/tools/dump_board_profiles.cpp):

```bash
bash firmware/test/boardcheck/run.sh            # regenerate in place
bash firmware/test/boardcheck/run.sh --check    # prove they still match (CI runs this)
```

Two hand-maintained copies of the same table always drift, and a drifted copy is
exactly what lets the wizard offer a pin the firmware then refuses — or, worse,
lets the firmware drive a pin the board uses for something else. So the drift is
made impossible instead of merely discouraged. Change `BoardProfile.cpp`, then
regenerate.

## Files

| File | Board | Source of truth |
| ---- | ----- | --------------- |
| `esp32-s3-devkitc-1.json` | ESP32-S3-DevKitC-1 **v1.0** (RGB LED on GPIO48) | `makeEsp32S3DevKitC1()` |
| `esp32-s3-devkitc-1-v1.1.json` | ESP32-S3-DevKitC-1 **v1.1** (RGB LED on GPIO38) | `makeEsp32S3DevKitC1V11()` |
| `esp32-wroom-32.json` | ESP32-WROOM-32 DevKitC (38-pin) | `makeEsp32Wroom32()` |
| `esp32-devkit-v1.json` | DOIT ESP32 DevKit v1 (30-pin) | `makeEsp32DevKitV1()` |

Espressif shipped two DevKitC-1 revisions that differ only in which GPIO carries
the on-board RGB LED — and that pin must stay reserved, so each revision is its
own profile. Check the silkscreen. The historical identifier
`esp32-s3-devkitc-1` keeps naming the **v1.0** board, so profiles stored before
the split keep exactly the pin map they were validated against.

The classic ESP32 boards have fewer free RMT/MCPWM units than the S3, so a
6-axis instrument is realistically an S3 job; they suit 1–3 strings. An axis that
cannot attach a hardware step generator is faulted at boot rather than silently
not stepping.

## Top-level format

```jsonc
{
  "identifier": "esp32-s3-devkitc-1",   // stable id, matches profile.board.profile
  "displayName": "ESP32-S3-DevKitC-1",
  "description": "…",
  "reference": "SPECIFICATION.md sections 11.4 / 11.5",
  "recommendedAssignment": { … },        // default auto-assign table (§11.5)
  "pins": [ { …PinCapability… }, … ]
}
```

### `recommendedAssignment`

The default, conflict-free assignment the "Assign pins automatically"
button proposes (SPECIFICATION.md §11.5). Array fields are indexed by
string number (1..6); the first *N* entries are used for an *N*-string
instrument.

| Key | Meaning | Value |
| --- | ------- | ----- |
| `STEP` | STEP outputs, strings 1..6 | `[4, 5, 6, 7, 15, 16]` |
| `DIR` | DIR outputs, strings 1..6 | `[17, 18, 8, 9, 10, 11]` |
| `HOME` | HOME sensors, strings 1..6 | `[12, 13, 14, 21, 38, 39]` |
| `SDA` | PCA9685 I²C data | `40` |
| `SCL` | PCA9685 I²C clock | `41` |
| `ENABLE` | global driver ENABLE | `42` |
| `SERVO_OE` | PCA9685 `/OE` safety line | `47` |

This is a starting profile, not a universal rule — the UI can override every
line (SPECIFICATION.md §11.5).

### `pins[]` — `PinCapability`

Each entry describes one physical GPIO. Fields match
`gmb::PinCapability` in `BoardProfile.h`:

| Field | Type | Meaning |
| ----- | ---- | ------- |
| `gpio` | int | GPIO number. |
| `exposed` | bool | Broken out on a board header. |
| `input` | bool | Usable as a digital input. |
| `output` | bool | Usable as a digital output. |
| `interrupt` | bool | Can raise a GPIO interrupt (needed for HOME/LIMIT/ESTOP). |
| `internalPullUp` | bool | Has a usable internal pull-up — required for the `ESTOP` input, which is sampled `INPUT_PULLUP`. |
| `internalPullDown` | bool | Has a usable internal pull-down. |
| `highSpeedOutput` | bool | Suitable for fast toggling / STEP generation. |
| `adc` | bool | Wired to an ADC channel. |
| `reserved` | bool | Reserved by firmware policy or hardware; never assignable. |
| `strapping` | bool | Boot-strapping pin (level sampled at reset). |
| `usb` | bool | Part of the USB-JTAG / native USB interface. |
| `onboardPeripheral` | bool | Wired to an on-board device (LED, UART header…). |
| `preference` | string | UI category: `"recommended"`, `"caution"`, or `"reserved"`. |
| `note` | string | Human-readable reason, shown in the UI. |

`preference` maps to the UI colours of SPECIFICATION.md §11.2:

* `recommended` → green — offered to beginners by default.
* `caution` → yellow — selectable, always shown with the `note` explanation.
* `reserved` → red — never selectable.

(The grey "already used" state of §11.2 is runtime state, not a static pin
property, so it does not appear here.)

## ESP32-S3-DevKitC-1 specifics

The profile encodes the ESP32-S3 restrictions of SPECIFICATION.md §11.4:

* **Strapping (reserved):** GPIO0 (also BOOT), GPIO3, GPIO45, GPIO46.
* **Native USB (reserved):** GPIO19 (D−), GPIO20 (D+) — kept free for a future
  USB transport.
* **Flash / PSRAM (reserved):** GPIO26–32.
* **Variant memory:** GPIO33/34 are **caution** (tied to memory on some
  modules); GPIO35/36/37 are **reserved** (octal Flash/PSRAM variants).
* **UART0 (reserved):** GPIO43 (TX), GPIO44 (RX) — programming / diagnostics.
* **On-board RGB LED (reserved):** GPIO48 on v1.0, GPIO38 on v1.1.
* **Recommended:** GPIO1, 2, 4–18, 21, 39–42, 47, plus whichever of 38/48 the
  board revision leaves free.

**GPIO22–25 do not exist on the ESP32-S3** and are intentionally absent from the
`pins` array.

## GPIO0 is reserved on every board

GPIO0 is the BOOT button, which the firmware also samples continuously: holding
it for ~2 s forces the Wi-Fi hotspot, so a wrong station configuration can never
lock you out ([`../docs/NETWORK_HOTSPOT.md`](../docs/NETWORK_HOTSPOT.md)). It is
therefore `reserved` on all four profiles — `pinSupports()`, `candidatesFor()`
and the validator all refuse it — because anything driven on that pin would fight
the escape hatch (and the bootloader entry).

## Classic ESP32 specifics

* **Input-only:** GPIO34, 35, 36 (VP), 39 (VN) — no output at all, so they can
  carry none of the output signals (STEP/DIR/ENABLE/I²C//OE), and they have no
  internal pull, so an endstop on one would need an external pull-up the wizard
  cannot verify. Marked `reserved`.
* **Strapping (caution):** GPIO2, 5, 12, 15.
* **Reserved:** GPIO1/3 (UART0), GPIO6–11 (SPI flash, 38-pin board only — the
  30-pin DevKit v1 does not break them out).
* **Absent:** GPIO20, 24, 28–31, 37, 38.
