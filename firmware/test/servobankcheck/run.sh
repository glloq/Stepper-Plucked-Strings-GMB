#!/usr/bin/env bash
# Runtime routing check for the two-I2C-bus ServoBank. Compiles the REAL
# src/platform/esp32/ServoBank.cpp against instrumented Wire / PCA9685 stubs (which
# record which bus each write went out on) plus the hostcheck Arduino stub, then
# runs it to prove bus-1 servos drive Wire1. Pure g++, no xtensa toolchain.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../.."            # firmware/
work="${TMPDIR:-/tmp}/gmb-servobankcheck"
mkdir -p "$work"

CXX="${CXX:-g++}"
# Our instrumented stubs (Wire.h, Adafruit_PWMServoDriver.h) win over the shared
# hostcheck stubs, which still supply Arduino.h and friends.
flags="-std=gnu++17 -Wall -Wextra -Wno-unused-variable -DARDUINO=300 \
  -DESP_ARDUINO_VERSION_MAJOR=3 -I$here/stubs -I$here/../hostcheck/stubs"

$CXX $flags "$here/main.cpp" "$root/src/platform/esp32/ServoBank.cpp" \
  -o "$work/servobankcheck"
"$work/servobankcheck"
