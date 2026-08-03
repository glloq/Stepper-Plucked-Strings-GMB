# PCB — Phase 5 deliverable

The integrated PCB is a **Phase 5 (dedicated hardware)** deliverable of the
SPECIFICATION.md (§24, §26). It is not yet designed; this directory is a
placeholder. The prototype build uses an ESP32-S3-DevKitC-1 with **pluggable
TMC2209 driver modules** and a PCA9685 breakout (§7.2) rather than a custom
board.

See `../schematics/README.md` for the schematic that the PCB will implement, and
`../wiring/WIRING.md` for the current reference interconnect.

## Planned contents

When produced, the PCB package will include:

* Board layout hosting the ESP32-S3 module, **1–6 pluggable TMC2209 sockets**,
  and the PCA9685 (on-board or headered).
* **Separated power planes/rails** (§22): 24 V motor, 5–7.4 V servo, 5 V logic,
  3.3 V, with a structured common-ground strategy.
* On-board **protection**: motor and servo fuses, reverse-polarity protection,
  TVS on the motor rail, per-driver decoupling, PCA9685 reservoir capacitor.
* **Lockable connectors** for motors, servos, sensors and power (§22).
* **Hardware E-stop** wiring: driver disable + PCA9685 `/OE` neutralisation,
  ESP32 kept alive (§21.2).
* Manufacturing outputs: Gerbers, drill files, assembly drawing, and a
  pick-and-place / BOM cross-reference to `../BOM.md`.
* Electrical validation notes (§24).
