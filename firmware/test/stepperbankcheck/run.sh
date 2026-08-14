#!/usr/bin/env bash
# Runtime check for the ARDUINO branch of StepperBank. Compiles the REAL
# src/platform/esp32/StepperBank.cpp against an instrumented FastAccelStepper stub
# (which records every engine call and can run out of step generators on demand)
# plus the shared hostcheck Arduino stub, then runs it — so hardStop vs
# controlledStopAll and the ActuatorResult contract are PROVEN, not just compiled.
# Pure g++, no xtensa toolchain.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../.."            # firmware/
work="${TMPDIR:-/tmp}/gmb-stepperbankcheck"
mkdir -p "$work"

CXX="${CXX:-g++}"
# Our instrumented FastAccelStepper.h wins over the shared hostcheck stub, which
# still supplies Arduino.h and friends.
flags="-std=gnu++17 -Wall -Wextra -Wno-unused-variable -DARDUINO=300 -DESP_ARDUINO_VERSION_MAJOR=3 \
  -I$here/stubs -I$here/../hostcheck/stubs"

$CXX $flags "$here/main.cpp" "$root/src/platform/esp32/StepperBank.cpp" \
  "$root/src/core/motion/StepperAxis.cpp" "$root/src/core/Types.cpp" \
  -o "$work/stepperbankcheck"
"$work/stepperbankcheck"
