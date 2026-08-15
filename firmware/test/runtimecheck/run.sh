#!/usr/bin/env bash
# Runtime checks for the two Arduino-gated components that own every mechanical
# decision: SafetySupervisor (arming / homing / hard stop / panic / axis faults)
# and PlaybackScheduler (the per-string release -> move -> press -> settle ->
# strike sequence).
#
# Both were compile-checked only — their own headers said so — which is a weak
# claim to make about the parts that decide when a carriage moves and what a fault
# cuts. Here the REAL components are built against the REAL StepperBank / ServoBank
# on top of the instrumented FastAccelStepper and PCA9685 stubs the bank harnesses
# already use, plus a controllable digitalRead()/millis() so a homing sequence is
# actually observable.
#
# Two binaries from one stub set: independent failures, no duplicated rig.
# Pure g++, no xtensa toolchain.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../.."            # firmware/
work="${TMPDIR:-/tmp}/gmb-runtimecheck"
mkdir -p "$work"

CXX="${CXX:-g++}"
# Instrumented stubs win over the shared hostcheck ones, which still supply
# Arduino.h (with the settable pin/clock table) and friends.
flags="-std=gnu++17 -Wall -Wextra -Werror -DARDUINO=300 -DESP_ARDUINO_VERSION_MAJOR=3 \
  -I$here/../stepperbankcheck/stubs -I$here/../servobankcheck/stubs \
  -I$here/../hostcheck/stubs"

# ArduinoJson, for the publish harness: ProfileStorage.h includes it even on the
# non-Arduino path. Same pinned release and same ARDUINOJSON_H override as
# hostcheck / profilecheck, so a runner that already has it does not fetch twice.
AJ="${ARDUINOJSON_H:-$work/ArduinoJson.h}"
if [ ! -f "$AJ" ]; then
  url="https://github.com/bblanchon/ArduinoJson/releases/download/v7.2.1/ArduinoJson-v7.2.1.h"
  echo "Downloading ArduinoJson -> $AJ"
  curl -fsSL --retry 3 --retry-delay 2 -o "$AJ.tmp" "$url"
  mv "$AJ.tmp" "$AJ"
fi
ajdir="$(dirname "$AJ")"

core="$root/src/core"
sources="\
  $root/src/platform/esp32/StepperBank.cpp \
  $root/src/platform/esp32/ServoBank.cpp \
  $core/Types.cpp \
  $core/board/BoardProfile.cpp \
  $core/board/PinManager.cpp \
  $core/configuration/Profile.cpp \
  $core/configuration/ProfileValidator.cpp \
  $core/instrument/InstrumentController.cpp \
  $core/instrument/StringController.cpp \
  $core/instrument/NoteAllocator.cpp \
  $core/midi/StringFretSelector.cpp \
  $core/motion/StepperAxis.cpp \
  $core/motion/MotionPlanner.cpp \
  $core/motion/HomingController.cpp \
  $core/safety/SafetyManager.cpp"

fail=0
for t in safety playback; do
  $CXX $flags "$here/main_$t.cpp" $sources -o "$work/runtimecheck_$t"
  "$work/runtimecheck_$t" || fail=1
done

# The publish transaction is built OFF-Arduino: WebApi::publishProfile lives outside
# the ARDUINO guard precisely so the ordering can be driven without ESPAsyncWebServer
# in the way, and the real ProfileStorage.cpp is deliberately left out so the harness
# can supply a storage that fails where it is told to.
#
# Its own freertos stubs come first: the shared ones are compile-only (a queue that
# never fills), and "the queue was full" is half of what this proves.
pubflags="-std=gnu++17 -Wall -Wextra -Werror \
  -I$here/stubs -I$ajdir -I$here/../hostcheck/stubs"
$CXX $pubflags "$here/main_publish.cpp" \
  "$root/src/platform/esp32/WebApi.cpp" \
  "$root/src/platform/esp32/ServoBank.cpp" \
  $core/Types.cpp \
  $core/board/BoardProfile.cpp \
  $core/board/PinManager.cpp \
  $core/configuration/Profile.cpp \
  $core/configuration/ProfileValidator.cpp \
  $core/instrument/InstrumentController.cpp \
  $core/instrument/StringController.cpp \
  $core/instrument/NoteAllocator.cpp \
  $core/midi/StringFretSelector.cpp \
  $core/motion/StepperAxis.cpp \
  $core/motion/MotionPlanner.cpp \
  $core/motion/HomingController.cpp \
  $core/safety/SafetyManager.cpp \
  $core/gmb/GmbSysExService.cpp \
  $core/gmb/GmbSysEx.cpp \
  $core/gmb/Capabilities.cpp \
  $core/gmb/GmbDescriptor.cpp \
  -o "$work/runtimecheck_publish"
"$work/runtimecheck_publish" || fail=1

exit $fail
