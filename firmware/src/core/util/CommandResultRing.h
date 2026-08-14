// Fixed-size ring tracking the outcome of the last N web->loop commands (audit
// P2.17 — extracted from main.cpp so the logic is host-testable and main.cpp shrinks).
//
// A web request enqueues a command and gets back an id; GET /api/commands?id=N then
// reports queued / succeeded / refused. Only the most recent kSize ids are retained
// (fixed memory on the ESP32); an id that has aged out reads back as "unknown".
//
// Pure C++17: the caller serialises access with its own lock (the ring itself does no
// locking, so it can be unit-tested on the host without FreeRTOS).
#pragma once

#include <cstdint>

namespace gmb {

class CommandResultRing {
public:
    // Cancelled = the command was purged from the queue WITHOUT running (panic /
    // E-stop). Distinct from Refused so a client awaiting an activation stops
    // immediately instead of polling a "queued" ghost to its timeout (audit 6).
    // Running / Failed close the async lifecycle for DEFERRED commands (audit 7):
    // a profile activation is Running from the moment its handler starts the
    // controlled park until the new profile really reaches Ready (-> Succeeded);
    // it becomes Failed when the swap aborts mid-flight (park unconfirmed, arming
    // failed) and Cancelled when a hard stop killed it. "succeeded" therefore
    // finally means "the new profile IS active", never "the procedure started".
    enum State : uint8_t {
        Queued = 0, Succeeded = 1, Refused = 2, Cancelled = 3, Running = 4, Failed = 5
    };

    // Record the outcome for `id` (ignored when id == 0). Updates in place if the id
    // is already tracked, otherwise takes the next ring slot (evicting the oldest).
    void set(uint32_t id, uint8_t state) {
        if (id == 0) return;
        for (auto& r : ring_)
            if (r.id == id) { r.state = state; return; }
        ring_[next_] = {id, state};
        next_ = (next_ + 1) % kSize;
    }

    // The outcome string for `id`: "queued" / "running" / "succeeded" / "refused" /
    // "cancelled" / "failed", or "unknown" if the id was never issued or has aged
    // out of the ring. id 0 (the "no command" sentinel, also the empty-slot value)
    // is always "unknown".
    const char* stateStr(uint32_t id) const {
        if (id == 0) return "unknown";
        for (const auto& r : ring_)
            if (r.id == id)
                return r.state == Queued ? "queued"
                     : r.state == Succeeded ? "succeeded"
                     : r.state == Cancelled ? "cancelled"
                     : r.state == Running ? "running"
                     : r.state == Failed ? "failed"
                     : "refused";
        return "unknown";
    }

private:
    struct Entry { uint32_t id = 0; uint8_t state = 0; };
    static constexpr int kSize = 16;
    Entry ring_[kSize];
    int next_ = 0;
};

}  // namespace gmb
