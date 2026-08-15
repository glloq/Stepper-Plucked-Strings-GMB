// Who may stage and commit /active.json, right now.
//
// There is exactly ONE temp file — /active.json.tmp — and therefore exactly one
// slot in which a snapshot write can be in progress. Two writers overlapping is
// not a race that produces a garbled file; LittleFS would happily serve both. It
// produces a file that is coherent and WRONG:
//
//     A: prepare (stages A)          B: prepare (stages B, over A's bytes)
//     A: commit  (renames B into place)
//     -> A is told its write succeeded, and /active.json contains B
//
// Publishing a profile got its own exclusion first, inside ActivationCoordinator.
// That was one writer guarding itself against copies of itself, which left
// POST /api/wifi — which writes the same snapshot, because the link config lives
// in its device half — as an unguarded second writer. This is that exclusion moved
// out to where both can take it: one lock per snapshot, not one per route.
//
// It is a try-lock, never a blocking one. A web handler that blocked here would
// hold an AsyncTCP task for the length of someone else's flash write; refusing and
// reporting back-pressure is both faster and truer — "another configuration write
// is in progress" is exactly what happened.
//
// Held across a flash write, and released by the SAME logical operation that took
// it. For the routes that can do it in one function, use Guard. The activation
// transaction cannot — its acquire and release live in different web callbacks —
// so it holds the raw lock and carries the invariant in its own documentation.
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace gmb {

class ActiveSnapshotLock {
public:
    void begin() { mutex_ = xSemaphoreCreateMutex(); }

    // Claim the right to write the snapshot. False = someone else has it.
    bool tryAcquire() {
        bool got = false;
        if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
        if (!held_) { held_ = true; got = true; }
        if (mutex_) xSemaphoreGive(mutex_);
        return got;
    }

    void release() {
        if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
        held_ = false;
        if (mutex_) xSemaphoreGive(mutex_);
    }

    bool busy() {
        if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
        bool b = held_;
        if (mutex_) xSemaphoreGive(mutex_);
        return b;
    }

    // RAII for the single-function case: acquire on construction, release on every
    // exit path including an early return. Check held() before doing anything.
    class Guard {
    public:
        explicit Guard(ActiveSnapshotLock& l) : lock_(l), held_(l.tryAcquire()) {}
        ~Guard() { if (held_) lock_.release(); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        bool held() const { return held_; }

    private:
        ActiveSnapshotLock& lock_;
        bool held_;
    };

private:
    SemaphoreHandle_t mutex_ = nullptr;
    bool held_ = false;
};

}  // namespace gmb
