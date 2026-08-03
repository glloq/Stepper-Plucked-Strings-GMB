#!/usr/bin/env bash
# Load every shipped instrument profile through the REAL firmware parser
# (ProfileStorage::fromJson) to guard the firmware<->web JSON contract (P0-1).
# Compiles the pure fromJson/toJson (non-Arduino path) against real ArduinoJson.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$here/../../.."          # repository root
work="${TMPDIR:-/tmp}/gmb-profilecheck"
mkdir -p "$work"

AJ="${ARDUINOJSON_H:-$work/ArduinoJson.h}"
if [ ! -f "$AJ" ]; then
  url="https://github.com/bblanchon/ArduinoJson/releases/download/v7.2.1/ArduinoJson-v7.2.1.h"
  echo "Downloading ArduinoJson -> $AJ"
  curl -sSL -o "$AJ" "$url"
fi
ajdir="$(dirname "$AJ")"

CXX="${CXX:-g++}"
flags="-std=gnu++17 -Wall -I$ajdir"

$CXX $flags \
  "$here/main.cpp" \
  "$repo/firmware/src/platform/esp32/ProfileStorage.cpp" \
  "$repo/firmware/src/core/configuration/Profile.cpp" \
  "$repo/firmware/src/core/configuration/ProfileValidator.cpp" \
  "$repo/firmware/src/core/board/BoardProfile.cpp" \
  "$repo/firmware/src/core/board/PinManager.cpp" \
  "$repo/firmware/src/core/motion/StepperAxis.cpp" \
  "$repo/firmware/src/core/Types.cpp" \
  -o "$work/profilecheck"

"$work/profilecheck" "$repo"
