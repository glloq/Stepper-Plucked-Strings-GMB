#!/usr/bin/env bash
# Host compile-check for the ESP32 platform layer.
#
# Compiles every src/platform/esp32/*.cpp and src/main.cpp with a host g++,
# against the REAL ArduinoJson single header plus minimal stub headers for the
# Arduino / async / driver libraries. This does NOT need the xtensa toolchain,
# so it runs in seconds and catches type / signature / ArduinoJson errors on
# every push (the class of error a native-core build cannot see).
#
# It is a compile check, not a substitute for the full `pio run` build.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../.."            # firmware/
stubs="$here/stubs"
work="${TMPDIR:-/tmp}/gmb-hostcheck"
mkdir -p "$work"

# Fetch the ArduinoJson single header (pinned) unless ARDUINOJSON_H is provided.
AJ="${ARDUINOJSON_H:-$work/ArduinoJson.h}"
if [ ! -f "$AJ" ]; then
  url="https://github.com/bblanchon/ArduinoJson/releases/download/v7.2.1/ArduinoJson-v7.2.1.h"
  echo "Downloading ArduinoJson -> $AJ"
  curl -sSL -o "$AJ" "$url"
fi
ajdir="$(dirname "$AJ")"

CXX="${CXX:-g++}"
flags="-std=gnu++17 -Wall -Wextra -DARDUINO=300 -DESP_ARDUINO_VERSION_MAJOR=3 -I$ajdir -I$stubs"

units=(
  src/main.cpp
  src/platform/esp32/ProfileStorage.cpp
  src/platform/esp32/WebApi.cpp
  src/platform/esp32/ServoBank.cpp
  src/platform/esp32/StepperBank.cpp
  src/platform/esp32/Net.cpp
  src/platform/esp32/MidiWifi.cpp
)

fail=0
for u in "${units[@]}"; do
  # Ignore warnings that originate in the stub headers themselves.
  if $CXX $flags -c "$root/$u" -o "$work/$(basename "$u").o" \
       2> "$work/err.txt"; then
    echo "[ OK ] $u"
  else
    echo "[FAIL] $u"
    cat "$work/err.txt"
    fail=1
  fi
done

exit $fail
