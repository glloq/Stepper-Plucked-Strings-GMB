# Opening and compiling the project in the Arduino IDE

The firmware can be built **either with PlatformIO or with the Arduino IDE** —
it is the same source code. This page describes the Arduino IDE path.

The `firmware/` folder is an Arduino *sketch*: it contains
[`firmware.ino`](../firmware/firmware.ino) (entry point, with the same name as
the folder) and a **`src/`** subfolder that the Arduino build processes
**recursively**. All of the firmware (pure C++ core + ESP32 adapters) is
therefore compiled automatically. `setup()` and `loop()` are in
`src/main.cpp`; the `.ino` file is intentionally left empty. The `test/` folder
(native tests) is ignored by the Arduino IDE.

---

## 1. Prerequisites

* **Arduino IDE 2.x** (recommended) — <https://www.arduino.cc/en/software>.
* The reference board **ESP32-S3-DevKitC-1** (or an equivalent ESP32-S3
  board).

## 2. Install ESP32 board support

1. `File ▸ Preferences`.
2. In **"Additional boards manager URLs"**, add:
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
3. `Tools ▸ Board ▸ Boards Manager…`, search for **esp32** and install
   **"esp32" by Espressif Systems** (version 3.x recommended: the servo LEDC
   driver and the `ledcAttach` API used here depend on it).

## 3. Install the libraries

`Tools ▸ Manage Libraries…`, then install:

| Library | Author / fork | Role |
| ------------ | ------------- | ---- |
| **ArduinoJson** (v7) | Benoît Blanchon | JSON profiles |
| **Adafruit PWM Servo Driver Library** | Adafruit | Servos via PCA9685 |
| **ESPAsyncWebServer** | ESP32Async (or `mathieucarbou`) | Web interface |
| **AsyncTCP** | ESP32Async | ESPAsyncWebServer dependency |

> Servos in **direct GPIO** mode use only the ESP32 core (LEDC); Adafruit
> PCA9685 is required only if you use at least one PCA9685. The other libraries
> are still required to compile.

## 4. Open the sketch

`File ▸ Open…` then select **`firmware/firmware.ino`**.
The IDE opens the sketch and shows `firmware.ino` as well as the `src/` tree.

## 5. Choose the board and its options

`Tools ▸ Board ▸ esp32 ▸ **ESP32S3 Dev Module**`, then set:

| Option | Recommended value |
| ------ | ----------------- |
| USB CDC On Boot | **Enabled** (serial console on the native USB) |
| Flash Size | **8MB** (or according to your module) |
| Partition Scheme | a scheme **with a filesystem**, e.g. *"8M with spiffs (3MB APP/1.5MB SPIFFS)"* |
| PSRAM | according to the module variant (OPI PSRAM if present) |
| Upload Mode | UART0 / Hardware CDC |

> **Reserved pins**: GPIO19/20 (native USB), 43/44 (UART0), 0/3/45/46
> (strapping), 48 (LED), 26–32 & 35–37 (Flash/PSRAM). The firmware and the Web
> interface exclude them automatically — see [`PIN_CONFIGURATION.md`](PIN_CONFIGURATION.md).

## 6. Compile and upload the firmware

Click **Verify** (✓) to compile, then **Upload** (→) with the board connected
via USB.

## 7. Upload the Web interface (LittleFS)

The interface is served from LittleFS (`/www`). It is uploaded separately:

1. Generate the filesystem image from `web-interface/`:
   ```bash
   cd firmware
   ./sync_web_data.sh        # copie web-interface/ -> firmware/data/www
   ```
   (On Windows without Bash: manually copy the contents of `web-interface/`
   into `firmware/data/www/`.)
2. Install the **arduino-littlefs-upload** plugin
   (<https://github.com/earlephilhower/arduino-littlefs-upload>): place the
   `.vsix` in `~/.arduinoIDE/plugins/` then restart the IDE.
3. `Ctrl/Cmd + Shift + P ▸ **Upload LittleFS to Pico/ESP8266/ESP32**`.

## 8. First startup

At power-on, the ESP32 creates the Wi-Fi access point
**`Stepper-Plucked-Strings-GMB`**. Connect to it and open
`http://192.168.4.1` to launch the configuration wizard
(see [`FIRST_CONFIGURATION.md`](FIRST_CONFIGURATION.md)).

---

## Troubleshooting

| Symptom | Cause / solution |
| -------- | ---------------- |
| `fatal error: ArduinoJson.h: No such file or directory` | Library not installed — see §3. |
| `ledcAttach was not declared` | ESP32 core is version 2.x — update to 3.x (§2). |
| Empty Web interface / 404 | LittleFS image not uploaded — redo §7 after `sync_web_data.sh`. |
| `Sketch too big` / no FS | Choose a *Partition Scheme* with a filesystem (§5). |
| The sketch does not compile the files in `src/` | Make sure you open `firmware/firmware.ino` (the `src/` must be **next to** the `.ino`). |
| Unit tests | They do **not** compile in the Arduino IDE; use `cd firmware/test && make` (see [`ARCHITECTURE.md`](ARCHITECTURE.md)). |

## PlatformIO equivalence

| Step | Arduino IDE | PlatformIO |
| ----- | ----------- | ---------- |
| Compile | Verify (✓) | `pio run` |
| Upload | Upload (→) | `pio run -t upload` |
| Filesystem | LittleFS plugin (§7) | `pio run -t uploadfs` |
| Serial monitor | Serial Monitor | `pio device monitor` |
