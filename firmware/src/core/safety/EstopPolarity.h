// E-stop pin polarity — the single place that turns a raw GPIO level into
// "is the emergency stop asserted?".
//
// This is one line of arithmetic, and it lived inline in main.cpp. That is
// exactly why it went wrong: `estopAsserted()` existed and was used by the
// continuous supervision in loop(), while beginHoming() and doReset() each kept
// their own `digitalRead(pin) == LOW` test from before the normally-closed wiring
// was supported. With the RECOMMENDED normally-closed chain that is inverted —
// a healthy loop reads LOW — so the pre-arm checks refused to home a perfectly
// safe machine and waved through an E-stop that was actually pressed.
//
// A duplicated safety predicate is a bug waiting to happen, and an Arduino-gated
// one cannot be unit-tested. So it lives here, in the pure core, with no
// dependency on the profile struct or on digitalRead(): every caller normalises
// through this function and the truth table is covered by host tests.
//
// The two supported wirings (hardware/POWER_AND_SAFETY.md):
//
//   normally-OPEN (legacy button): the button shorts the pin to GND, so
//     LOW = pressed = stop, HIGH (pull-up) = released = run.
//
//   normally-CLOSED (recommended): a closed loop to GND holds the pin LOW to
//     AUTHORISE running, so a press, a cut wire, a corroded contact or an
//     unplugged connector ALL read HIGH (pull-up) = stop. Losing the chain then
//     fails safe instead of silently disarming the E-stop — which is the whole
//     reason to prefer this wiring.
#pragma once

namespace gmb {

// True when the emergency stop is asserted (the machine must not run).
// `pinHigh` is the raw pin level; `normallyClosed` is the declared wiring.
constexpr bool estopAssertedFor(bool pinHigh, bool normallyClosed) {
    return normallyClosed ? pinHigh : !pinHigh;
}

}  // namespace gmb
