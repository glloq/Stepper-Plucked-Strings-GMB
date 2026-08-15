# Wiring guide

Connection guide for the **Stepper-Plucked-Strings-GMB** reference electronics
(SPECIFICATION.md §7 and §22). Default GPIO come from the ESP32-S3-DevKitC-1
board profile (§11.5, `board-profiles/esp32-s3-devkitc-1.json`); every line can
be reassigned from the web interface.

> ⚠️ Wire and power-check the machine unpowered, with drivers **disabled** and
> the PCA9685 `/OE` **high** (servos off). The firmware boots into a safe state:
> drivers off, servos neutralised, aux outputs cut (§21.1).

## 1. Default GPIO map (ESP32-S3-DevKitC-1)

| Function | Strings 1 → 6 (GPIO) |
| -------- | -------------------- |
| STEP | 4, 5, 6, 7, 15, 16 |
| DIR | 17, 18, 8, 9, 10, 11 |
| HOME | 12, 13, 14, 21, 38, 39 |

| Single signal | GPIO |
| ------------- | :--: |
| I²C SDA | 40 |
| I²C SCL | 41 |
| Global driver ENABLE | 42 |
| PCA9685 `/OE` (servo safety) | 47 |

Reserved / do-not-use on this board: GPIO0/3/45/46 (strapping), 19/20 (future
USB), 26–32 (Flash/PSRAM), 35/36/37 (variant memory), 33/34 (caution), 43/44
(UART0 programming/diagnostics), 48 (RGB LED). GPIO22–25 do not exist on the
ESP32-S3.

## 2. Stepper drivers (TMC2209, one per string)

Each pluggable driver module connects as follows:

| Driver pin | Connect to | Notes |
| ---------- | ---------- | ----- |
| `STEP` | ESP32-S3 STEP GPIO (per string) | fast digital output |
| `DIR` | ESP32-S3 DIR GPIO (per string) | direction |
| `EN` (`/ENABLE`) | Global ENABLE (GPIO42) | active-low; one line drives all drivers |
| `VM` / `GND` | 24 V motor rail / common ground | see Power |
| `VIO` | 3.3 V logic | driver logic reference |
| `A1 A2 B1 B2` | stepper motor coils | check motor datasheet pairing |
| `DIAG` (optional) | spare input GPIO | TMC2209 stall/diag |
| `UART` (optional) | spare UART | TMC2209 configuration |
| `MS1 / MS2` | set per module | microstep select (match `microsteps` in the profile) |

### Endstops: HOME and LIMIT

Each axis has a **HOME** sensor — the reference it must find before it may play —
and, strongly recommended, a **LIMIT** at the far end that catches a missed HOME.
Both come in two technologies, chosen per sensor in the profile
(`homing.homeSensor` / `homing.limitSensor`, Setup ▸ Homing in the web UI), and
they are wired differently:

| | Mechanical switch | Optical gate |
| --- | --- | --- |
| Wires | **2** — signal, GND | **3** — 3V3, GND, OUT |
| Supply | none (dry contact) | 3.3 V **from the ESP32**, never the motor rail |
| Firmware pull-up | enabled (`INPUT_PULLUP`) | enabled — harmless on a push-pull output, required on an open-collector one |
| Debounce | 3 ms, to let the contact settle | **none** — there is no contact to bounce |
| Typical repeatability | tens of µm, and it drifts as the contact wears | a few µm |

The debounce is not free. The zero is latched when the sensor trips during the
**slow seek**, so a 3 ms settling time at a slow-seek speed of *v* mm/s places
the zero *v*·0.003 mm past the real trigger point — 15 µm at the default 5 mm/s.
That offset is repeatable, but it **moves when the slow-seek speed is retuned**,
which is the confusing kind of error: change one speed and every fret shifts. An
optical gate removes it rather than calibrating around it, which is the whole
reason to fit one. The web UI shows the figure for the speed actually configured
(Setup ▸ Homing, and the Stepper drivers table on the Wiring tab).

Wiring notes for both:

* Wire a mechanical switch **normally-closed to the common** where the switch
  allows it, so a broken wire or an unplugged connector reads *triggered* rather
  than *clear*. The `sensorActiveHigh` / `limitActiveHigh` fields select the
  active level; NO and NC are both supported (§5 wizard, §13).
* **Check an optical module is a 3.3 V part.** A 5 V push-pull output on an
  ESP32 input is over its absolute maximum and needs a divider or a level
  shifter. Its output usually idles the **opposite** way from the switch it
  replaces, so set the active level from a real reading — Setup ▸ Homing ▸ *Test
  endstop* reports the raw pin level next to the declared one — rather than from
  the switch it replaced.
* Route the endstop wires **away from the motor leads**. A chopping driver puts
  fast edges on those four wires, and a sensor line running beside them down a
  moving cable chain is the usual cause of homing that works on the bench and
  trips at random once the machine is closed up. Use shielded or twisted pairs if
  they must share the chain.
* Both wire on interrupt-capable GPIO. Input-only pins (34/35/36/39 on a classic
  ESP32) are fine for endstops — they cannot drive anything, and an endstop only
  ever reads.

**Set the driver motor current** on each TMC2209 (VREF / UART) before enabling.

## 3. PCA9685 servo expander (I²C)

| PCA9685 pin | Connect to | Notes |
| ----------- | ---------- | ----- |
| `SDA` | GPIO40 | I²C data |
| `SCL` | GPIO41 | I²C clock |
| `VCC` | 3.3 V | chip logic |
| `V+` | 5–7.4 V servo rail | **separate** servo supply, not the ESP regulator |
| `GND` | common ground | shared with logic and servo supply |
| `/OE` | GPIO47 | **safety**: drive high to disable all outputs |

Pull-ups on SDA/SCL (2.2 kΩ–4.7 kΩ to 3.3 V) — many PCA9685 breakouts include
them. A **bulk reservoir capacitor** (≥ 470 µF) sits across `V+`/`GND` next to
the board (§22).

### `/OE` safety behaviour

`/OE` is active-low output-enable. Firmware holds it **high (outputs off)** at
boot and during panic/E-stop so no servo can move; it is pulled low only when the
configuration is validated and servos are armed (§21.1–21.3). A hardware E-stop
may also force `/OE` high directly.

### Servo channel map (§7.3)

| Channel | Function | Servo config `function` |
| :-----: | -------- | ----------------------- |
| 0–5 | finger press (one per string) | `finger` |
| 6–11 | individual pluck (one per string) | `pluck` |
| 12–15 | dampers / auxiliary | `damper`, `sharedDamper`, `aux` |

In the example profiles: finger servos on channels `0 … N−1`, pluck servos on
`6 … 6+N−1`.

## 4. Power rails (§22)

| Rail | Feeds | Source |
| ---- | ----- | ------ |
| **24 V** | stepper motors (through the drivers) | dedicated PSU |
| **5–7.4 V** | servomotors (PCA9685 `V+`) | **separate** servo PSU/BEC |
| **5 V** | logic | buck from 24 V or its own supply |
| **3.3 V** | ESP32-S3, driver `VIO`, sensors | ESP board regulator |

Mandatory measures:

* **Separate servo supply** — no servo is ever powered from the ESP32 regulator.
* **Fuse the motor rail** and **fuse the servo rail** independently.
* **Reverse-polarity protection** on the incoming supply.
* **TVS diode** across the 24 V motor rail (transient clamp).
* **Decoupling capacitors** close to each driver module (electrolytic + ceramic).
* **Bulk reservoir capacitor** near the PCA9685 `V+`.
* **Structured common ground** — star/plane ground tying all rails at one point.
* **Lockable connectors** on motor, servo and power harnesses.

## 5. Grounding & signal integrity

* Single, structured common ground reference for 24 V return, 5 V, 3.3 V and
  signal grounds.
* Keep STEP/DIR runs short; twist motor phase pairs; route them away from the
  HOME sensor and I²C wiring.
* Keep I²C (SDA/SCL) short or add stronger pull-ups; a bulk cap stabilises the
  servo rail against inrush when several servos move together.

## 6. Bring-up checklist

1. Wire everything with all supplies **off**.
2. Continuity-check grounds and confirm no rail-to-rail shorts.
3. Power **logic/3.3 V only**; confirm the ESP32-S3 boots and serves the web UI.
4. Power the **servo rail**; with `/OE` high, verify no servo twitches, then arm
   and test one finger servo.
5. Set each **driver current**, power the **24 V** rail, and home one axis at low
   speed before enabling the rest.
6. Verify the **STOP / panic** path disables drivers and forces `/OE` high.
