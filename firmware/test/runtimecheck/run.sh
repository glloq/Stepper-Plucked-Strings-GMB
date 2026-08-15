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
exit $fail
