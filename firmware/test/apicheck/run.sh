#!/usr/bin/env bash
# REST contract check between web-interface/js/api.js and the firmware's WebApi.cpp.
#
# This began as a route-NAME comparison, which catches one shape of defect: the UI
# calls a route the firmware does not serve (Settings > Security called
# POST /api/midi/source for months; the browser mock answered happily, the real
# device 404ed). It could not catch the other shape:
#
#     /api/pins/auto EXISTS, the UI sends `board`, and the handler ignores it — so
#     the offline demo produced a correct pin map and the real device handed out
#     pins for the wrong chip.
#
# Both compile. Both pass the unit tests. Both pass the browser smoke test, because
# the mock backend is what answers there. So the check is now driven by
# apicheck/contract.tsv and also verifies the METHOD each side uses and that every
# declared request field is read and every declared response field is written.
#
# The checker itself is mutation-tested: reintroducing the pins/auto defect, or
# changing a method or dropping a response field on one side, each fail it.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
exec python3 "$here/check.py" "$@"
