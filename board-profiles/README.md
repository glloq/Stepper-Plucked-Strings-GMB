# Board profiles

Machine-readable GPIO capability maps for the ESP32 boards supported by
**Stepper-Plucked-Strings-GMB**. The web configurator and the firmware pin
manager use these files to filter which GPIO may carry which signal, per board
and per module variant (cahier des charges §11).

Each JSON file mirrors the corresponding built-in profile in the firmware
(`firmware/src/core/board/BoardProfile.cpp`). Keeping the JSON and the C++ in
sync means the browser UI and the on-device validator agree about every pin.

## Files

| File | Board | Source of truth |
| ---- | ----- | --------------- |
| `esp32-s3-devkitc-1.json` | Espressif ESP32-S3-DevKitC-1 | `makeEsp32S3DevKitC1()` |

## Top-level format

```jsonc
{
  "identifier": "esp32-s3-devkitc-1",   // stable id, matches profile.board.profile
  "displayName": "ESP32-S3-DevKitC-1",
  "description": "…",
  "reference": "cahier des charges.md sections 11.4 / 11.5",
  "recommendedAssignment": { … },        // default auto-assign table (§11.5)
  "pins": [ { …PinCapability… }, … ]
}
```

### `recommendedAssignment`

The default, conflict-free assignment the "Attribuer automatiquement les
broches" button proposes (cahier des charges §11.5). Array fields are indexed by
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
line (cahier des charges §11.5).

### `pins[]` — `PinCapability`

Each entry describes one physical GPIO. Fields match
`gmb::PinCapability` in `BoardProfile.h`:

| Field | Type | Meaning |
| ----- | ---- | ------- |
| `gpio` | int | GPIO number. |
| `exposed` | bool | Broken out on a board header. |
| `input` | bool | Usable as a digital input. |
| `output` | bool | Usable as a digital output. |
| `interrupt` | bool | Can raise a GPIO interrupt (needed for HOME/LIMIT). |
| `highSpeedOutput` | bool | Suitable for fast toggling / STEP generation. |
| `adc` | bool | Wired to an ADC channel. |
| `reserved` | bool | Reserved by firmware policy or hardware; never assignable. |
| `strapping` | bool | Boot-strapping pin (level sampled at reset). |
| `usb` | bool | Part of the USB-JTAG / native USB interface. |
| `onboardPeripheral` | bool | Wired to an on-board device (LED, UART header…). |
| `preference` | string | UI category: `"recommended"`, `"caution"`, or `"reserved"`. |
| `note` | string | Human-readable reason, shown in the UI. |

`preference` maps to the UI colours of cahier des charges §11.2:

* `recommended` → green — offered to beginners by default.
* `caution` → yellow — advanced mode only, shown with the `note` explanation.
* `reserved` → red — never selectable.

(The grey "already used" state of §11.2 is runtime state, not a static pin
property, so it does not appear here.)

## ESP32-S3-DevKitC-1 specifics

The profile encodes the ESP32-S3 restrictions of cahier des charges §11.4:

* **Strapping (reserved):** GPIO0 (also BOOT), GPIO3, GPIO45, GPIO46.
* **Native USB (reserved):** GPIO19 (D−), GPIO20 (D+) — kept free for a future
  USB transport.
* **Flash / PSRAM (reserved):** GPIO26–32.
* **Variant memory:** GPIO33/34 are **caution** (tied to memory on some
  modules); GPIO35/36/37 are **reserved** (octal Flash/PSRAM variants).
* **UART0 (reserved):** GPIO43 (TX), GPIO44 (RX) — programming / diagnostics.
* **On-board RGB LED (reserved):** GPIO48.
* **Recommended:** GPIO1, 2, 4–18, 21, 38, 39, 40, 41, 42, 47.

**GPIO22–25 do not exist on the ESP32-S3** and are intentionally absent from the
`pins` array.
