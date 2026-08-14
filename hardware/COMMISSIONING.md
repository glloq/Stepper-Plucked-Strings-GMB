# Commissioning — electrical acceptance procedure

Staged power-up of a freshly wired (or re-wired) machine. It covers both halves
of this build: the servo side (fingers and plectrums) and the motion side (one
STEP/DIR driver and carriage per string). Each stage has a
**gate**: do not pass it until every check holds. Instruments: a multimeter;
a current clamp or bench PSU with current readout is strongly recommended, a
scope useful at stage 6. Circuit references:
[`POWER_AND_SAFETY.md`](POWER_AND_SAFETY.md) and
[`schematics/`](schematics/README.md).

> 💻 The web interface mirrors this procedure as a live checklist (*Wiring &
> GPIO → Commissioning*), with per-instrument progress kept in the browser.
> This document stays the reference — it carries the measurements to record.

## Stage 0 — visual & mechanical (all supplies OFF)

- [ ] Every servo lead, motor lead, power branch and the I²C harness routed and
      strain-relieved; lockable connectors latched.
- [ ] PCA9685 **A0–A2 jumpers** match the wiring sheet (bus + address per board).
- [ ] Driver **microstepping jumpers** (MS1–MS3, or the TMC UART setting) match
      the profile's `microsteps` — a mismatch scales every fret by the same
      wrong factor.
- [ ] Each carriage slides **freely by hand** over its whole travel: no binding,
      no cable snagging at either end, belt tension / screw backlash set,
      pulleys and couplers tight on their shafts.
- [ ] **HOME and LIMIT endstops at OPPOSITE ends** of each axis, and actually
      actuated by the carriage (push it by hand and watch the switch).
- [ ] Branch fuses F1…Fn (servo) and F1m…Fnm (motor) installed and rated per the
      sheet; F0 and F0m in place.
- [ ] E-stop button mounted, reachable, and **latched released**.
- [ ] No string under tension yet.

## Stage 1 — continuity & shorts (all supplies OFF)

- [ ] Continuity: every GND (ESP32, PCA boards, PSU, distribution block) is one net.
- [ ] **No** continuity between `+V_SERVO` and GND, `+V_MOTOR` and GND, `3V3`
      and GND, or between any two of `+V_SERVO` / `+V_MOTOR` / `3V3`.
- [ ] E-stop chain: NC #1 / NC #2 (/ NC #3) all **closed** with the button
      released, all **open** with it pressed.
- [ ] `/OE` bus: continuity from the pull-up node to every board's `/OE` pin;
      pull-up to 3.3 V present (measure ≈ R_pu to the 3V3 net, supplies off).
- [ ] `ENABLE` bus: continuity to every driver's `/EN` pin; **pull-up to 3.3 V
      present** — without it a floating `/EN` reads LOW on most drivers, so the
      coils energise with the ESP32 absent.
- [ ] Each driver's `STEP`/`DIR` pair goes to the axis it is labelled for.
      Swapped pairs are the classic first-run fault and are invisible until a
      carriage moves.
- [ ] HOME/LIMIT inputs read the expected level released, and the **opposite**
      level when the endstop is pressed by hand — per axis, both switches.
      Record the polarity you measure against the profile's
      `sensorActiveHigh` / `limitActiveHigh`.

## Stage 2 — logic only (USB in, servo PSU OFF)

- [ ] ESP32-S3 boots, web UI reachable; load or build the profile.
- [ ] *Wiring & GPIO* validation: **no errors** (SDA/SCL per used bus, `/OE`,
      ESTOP if installed, no GPIO conflicts).
- [ ] With the profile loaded but **not armed**: `/OE` bus measures **HIGH**
      (≈3.3 V) — servo outputs disabled — and the `ENABLE` bus measures **HIGH**
      too, so the drivers are released.
- [ ] Every enabled axis reports a **step generator attached** (no attach fault
      in the fault log at boot). On a classic ESP32 the RMT/MCPWM units are
      fewer than on the S3 — this is where a too-small board shows up.
- [ ] Press the E-stop: the UI shows `EmergencyStop` (ESTOP input works);
      release + `Reset` clears it. With **NC** wiring, unplug the chain
      connector instead: same result (fail-safe check).
- [ ] I²C scan (arming attempt is enough): every configured board answers at
      its (bus, address); a missing board is reported by identity.

## Stage 3 — both rails on, outputs disabled (do not arm)

- [ ] Servo rail voltage at the distribution block and at **each branch** within
      spec (5–6 V, correct polarity).
- [ ] Motor rail voltage at **each driver's `VMOT` terminals** within spec
      (12–24 V, correct polarity).
- [ ] **Set and MEASURE each driver's Vref** to its motor's rated phase current
      before anything moves. Do not trust the pot position. Too high cooks the
      motor and the driver; too low loses steps — and a lost step is a note at
      the wrong pitch that no software can detect.
- [ ] `/OE` still HIGH and `ENABLE` still HIGH: **no servo twitches**, and **no
      holding torque** on any motor (try to turn a shaft by hand — it must be
      free).
- [ ] Idle current plausible on both rails (boards' + drivers' quiescent draw
      only). The **motors must be cold**: any warmth here means the coils are
      energised, i.e. `ENABLE` is not doing its job.
- [ ] Press the E-stop: **K1 and K2 drop both rails** — 0 V at every branch,
      servo and motor. Release, re-close.

## Stage 4 — first motion, one axis (strings still off)

Arming has a fixed order, and watching it is itself a test: the **fingers lift
first** (a governed park, so they never all fire at once), and only then may a
carriage move. A carriage that starts seeking while a finger is still down is a
wiring or profile fault — stop and find it.

- [ ] Arm from the UI: fingers lift → carriages seek HOME → `Ready`.
- [ ] Each axis moves **TOWARD its HOME sensor** on the seek. An axis that runs
      to the far end has `DIR` inverted: stop immediately, fix `invertDirection`
      in the profile (or swap one motor phase pair) — do **not** move the
      sensor to match.
- [ ] HOME found, back-off + offset applied, position reads ≈ 0 mm at the
      reference.
- [ ] **Jog one axis a known distance and MEASURE it** (Instrument page → Jog).
      If 50 mm on screen is not 50 mm on the machine, the transmission
      (steps/mm: microsteps, pulley teeth, belt pitch or screw lead) is wrong,
      and *every* fret will be wrong by the same ratio. Fix it before anything
      else.
- [ ] Move **one finger servo** at low speed; verify direction, travel, rest.
- [ ] While a carriage is moving, press the E-stop: motion dies instantly
      (both rails cut + `/OE` and `ENABLE` HIGH + firmware latch), and the
      motor goes **free** (no holding torque). `Reset` + re-home afterwards
      works — recovery always re-homes, because a released driver has lost the
      position reference.
- [ ] Repeat the E-stop test with a **direct-GPIO servo** if any — this is the
      case only the power cut protects.

## Stage 5 — per-axis / per-branch bring-up

For each axis and each servo branch:

- [ ] The axis reaches **fret 0 and its highest fret** without hitting a soft
      limit or a LIMIT endstop.
- [ ] Fret positions checked against the real fretboard: adjust
      `fretOffsetMm` per string first (it shifts the whole fretboard from the
      HOME endstop), then calibrate individual frets only if the instrument's
      geometry genuinely departs from the equal-tempered law.
- [ ] **Repeatability**: move to the same fret 20× from alternating directions
      and confirm the carriage returns to the same place. Drift means lost
      steps — raise Vref, lower the acceleration, or fix the mechanics.
- [ ] Each servo on the branch reaches its rest and press/pluck positions
      (calibration steps), no binding, no chatter at rest (`disableAtRest`).
- [ ] Branch current at worst realistic case (chord on that string) within the
      branch fuse's continuous rating; note the peak.
- [ ] Servo branch voltage sag during the peak acceptable (< ~5 %); if not:
      bigger bulk cap, shorter/thicker wiring, or lower the per-board cap.
- [ ] **Motor rail sag during a simultaneous multi-axis acceleration** < ~5 %;
      if not: bigger cap at the driver, thicker wiring, or lower
      `power.maxConcurrentMoves` so fewer carriages start together.
- [ ] **Driver temperature** after a sustained passage is within the
      heatsink's comfort. The holding current runs for the whole time the
      instrument is armed, not only while moving.

## Stage 6 — whole instrument

- [ ] Full-instrument stress pattern (dense chords across all strings);
      PSU current + sag within spec; nothing warm beyond reason (fuses,
      connectors, wiring).
- [ ] Wi-Fi loss during play: notes release, instrument stays armed.
- [ ] Kill one branch fuse (or unplug one PCA) during play: the affected
      strings fault (`readyDegraded`), the rest keeps playing; global panic
      only with no strings left.
- [ ] Trigger a **LIMIT endstop by hand** during play: only that axis leaves
      service (`readyDegraded`), the others keep playing.
- [ ] E-stop at full load: instant, complete stop on both rails, every motor
      free; recovery = release + reset + **re-home** + re-arm.
- [ ] Only now: string the instrument and re-run stages 4–6 checks that
      involve motion, at low velocity first.

## Record

Keep with the machine: the wiring sheet (SVG export from the Wiring tab), fuse
ratings, measured idle/peak currents per branch and on the motor rail, **the
measured Vref of every driver**, the measured steps/mm per axis, PSU models,
capacitor values fitted, and the date of this procedure.
