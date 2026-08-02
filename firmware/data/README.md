# `data/` — LittleFS image

The firmware serves the web interface from the ESP32's LittleFS filesystem at
`/www`. Both the Arduino IDE filesystem uploader and PlatformIO's `uploadfs`
target upload the contents of this **`data/`** folder to LittleFS.

`data/www/` is **generated** from [`../../web-interface/`](../../web-interface/)
(the single source of truth) and is git-ignored. Regenerate it before uploading
the filesystem:

```bash
cd firmware
./sync_web_data.sh
```

Then upload the filesystem:

- **Arduino IDE 2.x** — install the *arduino-littlefs-upload* plugin, then run
  `Ctrl/Cmd+Shift+P ▸ Upload LittleFS to Pico/ESP8266/ESP32`.
- **PlatformIO** — `pio run -t uploadfs`.

See [`../../docs/ARDUINO_IDE.md`](../../docs/ARDUINO_IDE.md) for the full
walkthrough.
