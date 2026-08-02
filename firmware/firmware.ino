/*
 * Stepper-Plucked-Strings-GMB — Arduino IDE entry point.
 *
 * This sketch is intentionally almost empty: setup() and loop() live in
 * src/main.cpp, and the whole firmware (pure core + ESP32 adapters) sits under
 * the src/ subfolder, which the Arduino build compiles recursively. The same
 * tree also builds with PlatformIO (see platformio.ini).
 *
 * ── Open in the Arduino IDE ────────────────────────────────────────────────
 *   1. File ▸ Open… ▸ select this file (firmware/firmware.ino).
 *   2. Install the ESP32 board package and the required libraries, then pick
 *      the board and settings — see docs/ARDUINO_IDE.md for the exact list.
 *   3. Upload the sketch, then upload the web UI to LittleFS (data/www) — the
 *      guide explains both steps.
 *
 * Do not add setup()/loop() here: they are defined once in src/main.cpp.
 */
