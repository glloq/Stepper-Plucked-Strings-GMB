# Specification — pointer

The authoritative **specification** for Stepper-Plucked-Strings-GMB is located
at the root of the repository:

- 📄 [`../SPECIFICATION.md`](../SPECIFICATION.md) — main
  specification (architecture, GPIO, homing, notes, servos, state machines,
  allocation, MIDI, safety, phases, deliverables).

This file is a simple pointer: the content is **not** duplicated here in order
to keep a single source of truth.

## Complementary specifications (repository root)

- 📄 [`../STRING_FRET_SELECTION.md`](../STRING_FRET_SELECTION.md) —
  explicit selection of the string and fret via MIDI CC (CC20/CC21).
- 📄 [`../SYSEX_CAPABILITIES.md`](../SYSEX_CAPABILITIES.md) —
  GMB protocol for automatic announcement of capabilities via SysEx (blocks 1/5/6/7/8).

## Derived documentation (`docs/` folder)

The following documents summarize and operationalize the specification, in
connection with the code implemented in `firmware/src/core/*`:

| Document | Objective | Specification references |
| -------- | ----- | ----------------------------- |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | modules, data flow, capabilities snapshot, phases | §23, §24 |
| [`PIN_CONFIGURATION.md`](PIN_CONFIGURATION.md) | configurable GPIO, board profiles, conflicts | §11 |
| [`WEB_INTERFACE.md`](WEB_INTERFACE.md) | interface levels, wizard, pages, REST/WS API | §9, §10, §18–20 |
| [`MIDI_PROTOCOL.md`](MIDI_PROTOCOL.md) | MIDI transport, string/fret selection, GMB SysEx | §8 + related specs |
| [`CALIBRATION.md`](CALIBRATION.md) | steps/mm, homing, frets, servos | §12–15 |
| [`SAFETY.md`](SAFETY.md) | safe states, panic, E-stop, power supply | §21, §22 |
| [`FIRST_CONFIGURATION.md`](FIRST_CONFIGURATION.md) | beginner's guide to the wizard | §26 |
