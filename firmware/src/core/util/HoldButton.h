// Long-press detector (audit P2.17 — extracted from main.cpp's BOOT-button logic so
// the timing is host-testable and main.cpp keeps only the GPIO read).
//
// Fires EXACTLY ONCE per press when the input has been held continuously for holdMs.
// Releasing and pressing again re-arms it. Pure C++17 (no Arduino), like Debounce.
#pragma once

#include <cstdint>

namespace gmb {

class HoldButton {
public:
    void configure(uint32_t holdMs) { holdMs_ = holdMs; }

    // Feed the current pressed state at nowMs. Returns true on the single tick where
    // the button crosses holdMs of continuous hold; false otherwise.
    bool update(bool pressed, uint32_t nowMs) {
        if (pressed && !wasDown_) { downAtMs_ = nowMs; fired_ = false; }  // press edge
        bool fire = false;
        if (pressed && !fired_ && (nowMs - downAtMs_) >= holdMs_) {
            fired_ = true;   // one-shot until released
            fire = true;
        }
        wasDown_ = pressed;
        return fire;
    }

private:
    uint32_t holdMs_ = 2000;
    uint32_t downAtMs_ = 0;
    bool wasDown_ = false;
    bool fired_ = false;
};

}  // namespace gmb
