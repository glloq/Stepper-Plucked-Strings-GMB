#!/usr/bin/env bash
# REST contract check: every route the web interface CALLS must EXIST in the
# firmware, and every route the firmware serves should be reachable from the UI.
#
# This exists because two shipped defects were of exactly this shape, and no
# other check could see them:
#
#   * Settings > Security called POST /api/midi/source for months. The browser
#     mock answered happily; the firmware had no such route, so on a real device
#     every one of those buttons 404ed.
#   * The UI sent `board` to /api/pins/auto (and its mock honoured it) while the
#     firmware hard-coded the ESP32-S3 profile — so the offline demo produced a
#     correct pin map and the real device handed out pins for the wrong board.
#
# Both compile. Both pass the unit tests. Both pass the browser smoke test,
# because the mock backend is the thing that answers there. The only way to catch
# them is to compare the two sides directly, which is what this does.
#
#   run.sh            -> check
#   run.sh --list     -> print both sides (for debugging a mismatch)
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$here/../../.."
api="$repo/web-interface/js/api.js"
web="$repo/firmware/src/platform/esp32/WebApi.cpp"

# Routes the UI calls: string literals passed to _call()/fetch in api.js. Query
# strings and path parameters are stripped to the registered prefix.
ui_routes() {
  grep -oE "'/(api|gmb)/[a-zA-Z0-9/_.-]*'" "$api" \
    | tr -d "'" | sed 's/?.*//' | sort -u
}

# Routes the firmware registers: the literal passed to server_->on(...) or to an
# AsyncCallbackJsonWebHandler constructor.
fw_routes() {
  grep -oE '"/(api|gmb)/[a-zA-Z0-9/_.{}-]*"' "$web" \
    | tr -d '"' | sort -u
}

if [ "${1:-}" = "--list" ]; then
  echo "== UI calls =="; ui_routes
  echo "== firmware serves =="; fw_routes
  exit 0
fi

fail=0
fwlist="$(fw_routes)"

while read -r r; do
  [ -z "$r" ] && continue
  # A trailing slash marks a path parameter (/api/board/<id>): the firmware
  # registers one concrete route per board, so match on the prefix.
  if [ "${r%/}" != "$r" ]; then
    if ! echo "$fwlist" | grep -q "^${r}"; then
      echo "MISSING in firmware: the UI calls ${r}<param> but WebApi.cpp registers nothing under it"
      fail=1
    fi
    continue
  fi
  if ! echo "$fwlist" | grep -qx "$r"; then
    echo "MISSING in firmware: the UI calls $r but WebApi.cpp does not serve it"
    fail=1
  fi
done <<< "$(ui_routes)"

# The other direction is a warning, not a failure: the firmware may legitimately
# serve a route no browser calls (a controller endpoint, a debug hook).
uilist="$(ui_routes)"
while read -r r; do
  [ -z "$r" ] && continue
  if ! echo "$uilist" | grep -qx "$r" && \
     ! echo "$uilist" | grep -q "^$(dirname "$r")/$"; then
    echo "note: firmware serves $r, which the web interface never calls"
  fi
done <<< "$fwlist"

[ "$fail" -eq 0 ] && echo "apicheck OK"
exit $fail
