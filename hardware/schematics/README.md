# Schematics — Phase 5 deliverable

The electronic schematic is a **Phase 5 (dedicated hardware)** deliverable of the
SPECIFICATION.md (§24, §26). It is not yet drawn; this directory is a
placeholder.

Until then, the reference wiring is fully described in text:

* Electronics overview — `../README.md` (SPECIFICATION.md §7)
* Connection guide, pinout and power rails — `../wiring/WIRING.md` (§7 / §22)
* Bill of materials — `../BOM.md`
* Default GPIO map — `../../board-profiles/esp32-s3-devkitc-1.json` (§11.5)

## Planned contents

When produced, the schematic set will capture:

* **ESP32-S3-DevKitC-1** connections with the GPIO assignment of §11.5.
* **TMC2209** driver sockets (1–6, pluggable): STEP/DIR/EN, HOME, optional
  LIMIT/DIAG/UART, motor phases, `VM`/`VIO` supplies, current-set network.
* **PCA9685** servo expander: I²C (SDA/SCL) with pull-ups, `V+` servo rail with
  bulk capacitor, all 16 channel headers, and the `/OE` safety line to GPIO47.
* **Sensor front-end**: HOME/LIMIT inputs with pull configuration.
* **Power tree** (§22): 24 V motor, 5–7.4 V servo (separate), 5 V logic, 3.3 V —
  with fuses (motor + servo), reverse-polarity protection, TVS on the motor
  rail, driver decoupling and PCA9685 reservoir cap.
* **Safety / E-stop** path: hardware disable of drivers and PCA9685 `/OE`
  (§21.2), ESP32 kept powered.
* Net labels and connector pinouts matching `../BOM.md` and `../wiring/WIRING.md`.
