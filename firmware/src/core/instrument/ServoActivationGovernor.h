// Servo start-permit governor — limits PCA9685 in-rush current.
//
// A hobby servo draws its peak current in the first few milliseconds of a move,
// when it slews hardest toward the new target. Re-fretting a whole chord at once
// would start many fingers together and stack those peaks, browning out the 5-6 V
// rail / tripping the PCA9685. This governor spreads the STARTS: it grants at most
// N start-permits within any rolling `staggerMs` window, so a burst of presses is
// fanned out over time instead of firing simultaneously.
//
// The cap is OPTIONAL and applied at two scopes, each independently:
//   • GLOBAL     — across the whole instrument (`maxConcurrent`, 0 = no limit).
//   • PER-BOARD  — per physical PCA9685 (`maxPerBoard`, 0 = no limit), passed as the
//                  board index on requestStart(). Each board has its own power input,
//                  so this bounds one board's in-rush even under the global cap.
// A start is granted only when BOTH the global and the board window have room. With
// both caps 0 (or staggerMs 0) the governor is off and every request is granted.
//
// It complements the two other current-limiting measures: per-servo disableAtRest
// (idle fingers cut their PWM entirely) and the release-before-press sequence (one
// finger holding per string at a time). Pure C++17, host-testable.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace gmb {

class ServoActivationGovernor {
public:
    static constexpr int kBoards = 16;  // (i2cBus 0/1) x (0x40..0x47) distinct boards

    // Global cap + stagger, no per-board limit (kept for existing callers/tests).
    // staggerMs == 0 disables throttling (every request is granted immediately).
    void configure(uint8_t maxConcurrent, uint16_t staggerMs) {
        configure(maxConcurrent, 0, staggerMs);
    }
    // Global cap (0 = no limit) + per-PCA-board cap (0 = no limit) + stagger window.
    void configure(uint8_t maxConcurrent, uint8_t maxPerBoard, uint16_t staggerMs) {
        staggerMs_ = staggerMs;
        global_.setCap(maxConcurrent);
        for (auto& b : board_) b.setCap(maxPerBoard);
    }

    void reset() {
        global_.clear();
        for (auto& b : board_) b.clear();
    }

    // Ask to start a servo move at nowMs on `board` (0..kBoards-1; 0xFF or out of
    // range = a servo on no PCA board, e.g. a direct GPIO — only the global cap
    // applies). Returns true and records the start if granted; false to wait+retry.
    bool requestStart(uint32_t nowMs, uint8_t board = 0xFF) {
        if (staggerMs_ == 0) return true;  // throttling disabled
        Window* bw = (board < kBoards) ? &board_[board] : nullptr;
        if (!global_.hasRoom(nowMs, staggerMs_)) { ++throttles_; return false; }
        if (bw && !bw->hasRoom(nowMs, staggerMs_)) { ++throttles_; return false; }
        global_.grant(nowMs);
        if (bw) bw->grant(nowMs);
        return true;
    }

    // Cumulative number of start requests deferred (throttled) by the governor — a
    // diagnostics counter, deliberately NOT cleared by reset() (audit P2.19).
    uint32_t throttleCount() const { return throttles_; }

    // FORECAST (audit 4 P1.3): milliseconds until ONE new start could be granted at
    // nowMs given the CURRENT window state — 0 = immediately. Pure query, nothing is
    // recorded. Lets the playback scheduler's chord-deadline maths account for
    // grants issued JUST BEFORE the chord instead of assuming an empty window (and
    // without re-implementing the governor's internals).
    uint32_t nextSlotDelayMs(uint32_t nowMs, uint8_t board = 0xFF) const {
        if (staggerMs_ == 0) return 0;
        uint32_t d = global_.slotDelayMs(nowMs, staggerMs_);
        if (board < kBoards) {
            uint32_t b = board_[board].slotDelayMs(nowMs, staggerMs_);
            if (b > d) d = b;
        }
        return d;
    }

    // FULL batch forecast (audit 5): simulate granting `count` starts on the boards
    // in `boards[]` (0xFF = no board), in order, starting from the governor's
    // CURRENT window state, and return how long after nowMs the LAST one begins.
    // Works on COPIES of the windows — nothing is recorded. This models a
    // partially-occupied sliding window exactly (historical grants expire at
    // different instants), which a single next-slot delay plus a closed-form queue
    // estimate cannot (the audit's cap-3 example: grants at t-7/t-1 make the 6th
    // start of a new batch wait 19 ms, not 10).
    uint32_t forecastBatchDelayMs(uint32_t nowMs, const uint8_t* boards,
                                  size_t count) const {
        if (staggerMs_ == 0 || count == 0) return 0;
        Window g = global_;
        std::array<Window, kBoards> b = board_;
        uint32_t t = nowMs, last = nowMs;
        for (size_t i = 0; i < count; ++i) {
            Window* w = boards[i] < kBoards ? &b[boards[i]] : nullptr;
            uint32_t start = t;  // starts are issued in order
            for (;;) {
                uint32_t d = g.slotDelayMs(start, staggerMs_);
                if (w) {
                    uint32_t d2 = w->slotDelayMs(start, staggerMs_);
                    if (d2 > d) d = d2;
                }
                if (d == 0) break;
                start += d;
            }
            g.grant(start);
            if (w) w->grant(start);
            if (start > last) last = start;
            t = start;
        }
        return last - nowMs;
    }

private:
    // A rolling window: at most `cap` grants within any staggerMs window. cap == 0
    // means unlimited (always room, records nothing).
    struct Window {
        void setCap(uint8_t c) { cap = c; recent.assign(c, 0); head = 0; filled = 0; }
        void clear() { std::fill(recent.begin(), recent.end(), 0u); head = 0; filled = 0; }
        bool hasRoom(uint32_t nowMs, uint16_t staggerMs) const {
            if (cap == 0) return true;                 // unlimited
            if (filled < cap) return true;             // window not yet full
            uint32_t oldest = recent[head];            // oldest of the last `cap` grants
            return static_cast<int32_t>(nowMs - oldest) >= static_cast<int32_t>(staggerMs);
        }
        // Forecast: ms until this window would grant one more start (0 = now).
        uint32_t slotDelayMs(uint32_t nowMs, uint16_t staggerMs) const {
            if (cap == 0 || filled < cap) return 0;
            int32_t wait = static_cast<int32_t>(recent[head] + staggerMs - nowMs);
            return wait > 0 ? static_cast<uint32_t>(wait) : 0;
        }
        void grant(uint32_t nowMs) {
            if (cap == 0) return;
            recent[head] = nowMs;
            head = (head + 1) % cap;
            if (filled < cap) ++filled;
        }
        uint8_t cap = 0;
        std::vector<uint32_t> recent;
        uint8_t head = 0, filled = 0;
    };

    uint16_t staggerMs_ = 8;
    Window global_;
    std::array<Window, kBoards> board_;
    uint32_t throttles_ = 0;  // cumulative deferred starts (diagnostics; survives reset)
};

}  // namespace gmb
