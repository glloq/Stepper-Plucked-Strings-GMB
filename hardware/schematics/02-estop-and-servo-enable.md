# Sheet 02 — E-stop chain, servo output-enable & driver ENABLE

Reference safety chain of this build (companion to
[`../POWER_AND_SAFETY.md`](../POWER_AND_SAFETY.md) §2–4). The E-stop is a
**latching NC mushroom button** with two (optionally three) independent
contacts; both `/OE` (servo side) and `ENABLE` (motor side) are made
**fail-safe** by a pull-up and, in the recommended variant, a gated
non-inverting enable stage.

Two things must go away on a stop, not one:

* the **servo rail** (K1) plus `/OE`, which covers the PCA servos — and, only
  through the power cut, the direct-GPIO ones;
* the **motor rail** (K2) plus the driver `ENABLE`, because a stepper holds its
  position by burning current: stopping the STEP pulses stops *motion*, but the
  coils stay energised, the driver stays hot and the carriage stays clamped
  until `ENABLE` goes inactive or the rail disappears.

NC #1 therefore carries **both contactor coils** (K1 and K2 in series, or one
contact per coil on a two-pole contact block), and NC #3 gates **both** enable
stages.

## 1. E-stop chain

```text
                       E-STOP BUTTON (latching, twist/pull release)
                 ┌───────────────┬───────────────────┬─────────────────┐
                 │  NC #1        │  NC #2            │  NC #3 (option) │
                 │  (power)      │  (status)         │  (enable gate)  │
                 │               │                   │
   +V control ───┤               │                   │
                 │  A1           │                   │
              ┌──●──────┐        │                   └── in series with Q2's
              │ K1 + K2 │        │                       ground return (§2 below)
              │  coils  │        │                       — one stage per bus for
              │ (servo  │        │                         /OE, one for ENABLE
              │         ▲ D_fw   │   D_fw is IN PARALLEL with the coil,
              │         │ (par-  │   REVERSE-biased in normal operation:
              └──●──────┘ allel) │     cathode (barre) → A1, the +V side
                 │  A2           │     anode           → A2, the GND side
                GND              │   It only conducts the coil's inductive
                                 │   kick when NC #1 opens — never in series
                                 │   with the coil (a series diode would just
                                 │   drop 0.7 V and protect nothing).
   ESP32 ESTOP GPIO ─────────────┤   NC loop: closed = pin LOW = run allowed
   (INPUT_PULLUP)                │   open (press / cut wire / unplugged)
                                 │        = pin HIGH = STOP (fail-safe)
                                GND
```

* **NC #1** switches only K1's **coil** — never the servo current itself. K1 is
  rated for the rail's **DC** current. `D_fw` (1N4007 class) sits **across the
  coil terminals A1/A2, cathode on the +V side**, so it clamps the inductive
  spike when the chain opens; K1's contact side (sheet 01) is untouched.
* **NC #2** is the firmware **status loop** on the `ESTOP` input (profile:
  *Emergency stop input*, contact = *Normally closed*). The firmware latches
  `EmergencyStop`; recovery needs the button released *and* `POST /api/reset`.
* **NC #3** (recommended) gates the `/OE` enable stage below so the PCA outputs
  cannot be re-enabled while the chain is open.

## 2. `/OE` fail-safe enable

Level 0 (mandatory): pull-up on the bus.

```text
3.3 V ── [R_pu 10 kΩ] ──●── /OE bus ──► PCA #0 /OE, PCA #1 /OE, … (chained)
                        │
              ESP32 SERVO_OE (LOW = enable, HIGH / high-Z = disabled)
```

The PCA9685's own `/OE` pin has an internal weak **pull-down** — floating
`/OE` = outputs **enabled**. The external pull-up outweighs it, so an absent,
resetting or unplugged ESP32 leaves every output disabled.

Level 1 (recommended): gated, non-inverting stage — full circuit, truth table
and resistor values in [`../POWER_AND_SAFETY.md`](../POWER_AND_SAFETY.md) §3.

```text
SERVO_OE ─[R1]─► Q1 (base pulled UP by R4)  — inverts, safe-on-idle
node A  ─[R3 pull-up]─► Q2 base
Q2: open collector on the /OE bus, emitter → NC #3 → GND
```

```text
SERVO_OE LOW + chain closed → ENABLED       SERVO_OE high-Z → disabled
SERVO_OE HIGH               → disabled      chain open      → DISABLED
```

Never tie `/OE` hard to 3.3 V through a switch while the GPIO drives it — that
is contention; inhibition goes through the gate or the power path.

## 3. Split `/OE` (two I²C buses)

With `SERVO_OE2` configured (bus 1 boards on their own safety line), duplicate
`R_pu` **and** the enable stage per bus; a single NC #3 contact gates both
stages' ground returns.

## 4. Coverage matrix

| Failure / action | K1 power cut | `/OE` chain | `ESTOP` input |
| ---------------- | :----------: | :---------: | :-----------: |
| Button pressed | ✔ rail dead | ✔ outputs off | ✔ firmware latches |
| Chain wire cut / unplugged | ✔ (NC) | ✔ (NC #3) | ✔ (NC loop) |
| ESP32 absent / resetting / power loss | — | ✔ (pin high-Z → pull-up) | n/a |
| Firmware **hard** hang (internal WDT fires → reset) | — | ✔ (reset → pin high-Z) | n/a |
| Firmware **soft** hang (CPU alive, logic stuck, pin still driven LOW) | ✘ **not guaranteed** | ✘ **not guaranteed** — the pull-up cannot beat a driven pin | ✘ |
| → the guaranteed stop for a soft hang | ✔ **the button** (NC #1) | ✔ the button (NC #3) | via button |
| Direct-GPIO servo runaway | ✔ **only this stops it** | ✘ no effect | status only |

A **hardware watchdog** (ESP32 heartbeat → external monostable → gates K1 and
the enable stage) would turn the soft-hang rows into ✔ — recommended for any
unattended or public installation, out of scope for the bench prototype.
