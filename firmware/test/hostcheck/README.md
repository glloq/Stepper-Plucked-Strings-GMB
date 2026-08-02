# Host compile-check for the ESP32 platform layer

`run.sh` compiles every `src/platform/esp32/*.cpp` and `src/main.cpp` with a
host `g++`, against the **real ArduinoJson** single header plus **minimal stub
headers** (`stubs/`) for the Arduino / async-web / driver libraries.

## Why

The native unit tests (`firmware/test/Makefile`) only build the pure core, so
they cannot catch compile errors in the ESP32 glue — e.g. an `std::string` vs
Arduino `String` mismatch, or an ambiguous ArduinoJson conversion. The full
`pio run` build catches those but downloads a large xtensa toolchain. This
check runs in seconds with just `g++`, so it gives fast feedback on the class
of error that only appears when the platform code is actually compiled.

```bash
cd firmware/test/hostcheck && ./run.sh
```

## What it is NOT

- It is **not** a substitute for `pio run`: the stubs approximate the library
  APIs the firmware calls, and only ArduinoJson is the real library. It checks
  compilation of each translation unit, not linking or image generation.
- If a real library changes a signature the stub doesn't mirror, only the full
  `pio run` job will catch it.

Both this check and the full `pio run` build run in CI (`.github/workflows/ci.yml`).
