#!/usr/bin/env bash
# Host compile-check for the ESP32 platform layer.
#
# Compiles every src/platform/esp32/*.cpp and src/main.cpp with a host g++,
# against the REAL ArduinoJson single header plus minimal stub headers for the
# Arduino / async / driver libraries. This does NOT need the xtensa toolchain,
# so it runs in seconds and catches type / signature / ArduinoJson errors on
# every push (the class of error a native-core build cannot see).
#
# It then LINKS every unit together against a tiny Arduino-style main. That second
# step is not cosmetic: compiling translation units one at a time cannot see an
# undefined reference, so three ProfileStorage methods once lost their Arduino
# bodies — leaving only the non-Arduino stubs — and this check stayed green while
# all four PlatformIO builds failed at the linker. A check that structurally cannot
# fail on a whole class of defect is worse than no check, because it is believed.
#
# It is still not a substitute for the full `pio run` build (no xtensa codegen, no
# real libraries), but "does it link" is now inside the fast loop rather than 20
# minutes downstream.
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
  # -f: an HTTP error is a failure, never save an error page AS the header (that is
  # what made the ArduinoJson types "undeclared" on a flaky runner). --retry: ride out
  # a transient network blip. Stage via .tmp + mv so a partial download is never cached.
  curl -fsSL --retry 3 --retry-delay 2 -o "$AJ.tmp" "$url"
  mv "$AJ.tmp" "$AJ"
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

# The pure core + the generated web assets. They are not compile-checked
# individually (the native test build already compiles the core with -Werror), but
# they must be present at link time or every core symbol comes up undefined and the
# real question — "is a platform method declared but never defined?" — drowns.
support=(
  src/core/Types.cpp
  src/core/board/BoardProfile.cpp
  src/core/board/PinManager.cpp
  src/core/configuration/Profile.cpp
  src/core/configuration/ProfileValidator.cpp
  src/core/gmb/Capabilities.cpp
  src/core/gmb/GmbDescriptor.cpp
  src/core/gmb/GmbSysEx.cpp
  src/core/gmb/GmbSysExService.cpp
  src/core/instrument/InstrumentController.cpp
  src/core/instrument/NoteAllocator.cpp
  src/core/instrument/StringController.cpp
  src/core/midi/MidiParser.cpp
  src/core/midi/StringFretSelector.cpp
  src/core/motion/HomingController.cpp
  src/core/motion/MotionPlanner.cpp
  src/core/motion/StepperAxis.cpp
  src/core/safety/SafetyManager.cpp
  src/platform/esp32/WebAssets.cpp
)

fail=0
objs=()
for u in "${units[@]}"; do
  o="$work/$(echo "$u" | tr / _).o"
  # Ignore warnings that originate in the stub headers themselves.
  if $CXX $flags -c "$root/$u" -o "$o" 2> "$work/err.txt"; then
    echo "[ OK ] $u"
    objs+=("$o")
  else
    echo "[FAIL] $u"
    cat "$work/err.txt"
    fail=1
  fi
done

for u in "${support[@]}"; do
  o="$work/$(echo "$u" | tr / _).o"
  if $CXX $flags -c "$root/$u" -o "$o" 2> "$work/err.txt"; then
    objs+=("$o")
  else
    echo "[FAIL] $u (support unit)"
    cat "$work/err.txt"
    fail=1
  fi
done

# The link. An Arduino sketch has no main(), so supply the one the core runtime
# would: call setup() once, then a bounded number of loop() passes. It is never
# RUN here — building the executable is the whole test — but it must be a real
# call so the linker actually resolves everything setup()/loop() reach.
if [ "$fail" -eq 0 ]; then
  cat > "$work/link_main.cpp" <<'EOF'
extern void setup();
extern void loop();
int main() {
    setup();
    for (int i = 0; i < 2; ++i) loop();
    return 0;
}
EOF
  if $CXX $flags -c "$work/link_main.cpp" -o "$work/link_main.o" 2> "$work/err.txt" &&
     $CXX "${objs[@]}" "$work/link_main.o" -o "$work/firmware_host" 2> "$work/err.txt"; then
    echo "[ OK ] link (every declared symbol has a definition)"
  else
    echo "[FAIL] link"
    cat "$work/err.txt"
    fail=1
  fi
fi

exit $fail
