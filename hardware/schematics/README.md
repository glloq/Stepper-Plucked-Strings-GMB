# Schematics

Reference schematic set for the stepper-carriage electronics. The sheets are
**text schematics** for now — precise enough to build and review the machine —
and are the source the future KiCad capture must match.

| Sheet | Contents |
| ----- | -------- |
| [`01-power-distribution.md`](01-power-distribution.md) | PSU, reverse-polarity protection, main fuse, master switch, E-stop contactor, star distribution, per-branch fuses & bulk capacitors |
| [`02-estop-and-servo-enable.md`](02-estop-and-servo-enable.md) | latching NC E-stop chain (power + status + enable-gate contacts), fail-safe `/OE` and driver-`ENABLE` pull-ups and their gated enable stages, coverage matrix |
| [`03-esp32-pca9685-one-string.md`](03-esp32-pca9685-one-string.md) | the servo side of one string: ESP32 ↔ PCA9685 signals, address jumpers, channel map, direct-GPIO servos |
| [`04-stepper-driver-one-axis.md`](04-stepper-driver-one-axis.md) | the motion side of one string: STEP/DIR driver, shared ENABLE, HOME/LIMIT endstops, motor rail, microstepping and Vref |

The architecture and sizing rules behind these sheets are in
[`../POWER_AND_SAFETY.md`](../POWER_AND_SAFETY.md); wire-by-wire connections in
[`../wiring/WIRING.md`](../wiring/WIRING.md); parts and statuses in
[`../BOM.md`](../BOM.md). Capacity limits (strings, boards per bus, channels)
are defined once in [`../README.md`](../README.md) §Capacity.

## KiCad capture (future)

When the KiCad set is drawn it must keep the net names of the sheets above
(`+V_SERVO`, `+V_MOTOR`, `+V_BR<n>`, `/OE bus`, `ENABLE bus`, `ESTOP loop`, …), the connector pinouts of
`../BOM.md`, and the default GPIO of the board profiles
(`../../board-profiles/`).
