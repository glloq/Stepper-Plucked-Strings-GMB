#!/usr/bin/env bash
# Regenerate board-profiles/*.json from the C++ board profiles, or (--check) prove
# the shipped JSON still matches them.
#
# The JSON board profiles are what the web wizard reads; BoardProfile.cpp is what
# the firmware validates against. Two hand-maintained copies of one table drift, and
# a drifted copy is what lets the wizard offer a pin the validator refuses. So the
# JSON is GENERATED, and CI runs this with --check.
#
#   run.sh          -> regenerate board-profiles/ in place
#   run.sh --check  -> generate into a temp dir and diff (non-zero on any difference)
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$here/../../.."          # repository root
work="${TMPDIR:-/tmp}/gmb-boardcheck"
mkdir -p "$work"

CXX="${CXX:-g++}"
$CXX -std=c++17 -Wall -Wextra -O1 \
  "$repo/firmware/tools/dump_board_profiles.cpp" \
  "$repo/firmware/src/core/board/BoardProfile.cpp" \
  -o "$work/dump_board_profiles"

if [ "${1:-}" = "--check" ]; then
  out="$work/generated"
  rm -rf "$out"
  mkdir -p "$out"
  "$work/dump_board_profiles" "$out" >/dev/null
  fail=0
  for f in "$out"/*.json; do
    name="$(basename "$f")"
    if ! diff -u "$repo/board-profiles/$name" "$f"; then
      echo "board-profiles/$name is out of date — run firmware/test/boardcheck/run.sh"
      fail=1
    fi
  done
  # A JSON with no C++ counterpart is just as wrong: the wizard would offer a board
  # the firmware cannot validate.
  for f in "$repo"/board-profiles/*.json; do
    name="$(basename "$f")"
    if [ ! -f "$out/$name" ]; then
      echo "board-profiles/$name has no BoardProfile.cpp counterpart"
      fail=1
    fi
  done
  [ "$fail" -eq 0 ] && echo "boardcheck OK"
  exit $fail
fi

"$work/dump_board_profiles" "$repo/board-profiles"
