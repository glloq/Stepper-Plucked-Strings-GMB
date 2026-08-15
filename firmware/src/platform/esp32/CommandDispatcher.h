// Web -> loop() command queue + dispatch, extracted from main.cpp.
//
// The web task never touches mechanical state directly (spec P0): ESPAsyncWebServer
// runs its callbacks in the AsyncTCP task, which can execute in parallel with loop().
// If those callbacks touched the profile / instrument / steppers / servos directly
// they could reallocate a std::vector while loop() is iterating it. So every mutating
// request only ENQUEUES a typed AppCommand and gets back an id; loop() is the SOLE
// owner of the mechanical state and drains a bounded batch each tick.
//
// This class owns all the plumbing — the FreeRTOS queue, the id counter, and the
// outcome ring (via the host-tested CommandResultRing under a mutex) — while the
// command HANDLERS stay in main.cpp, where the instrument / servos / safety live,
// and are injected as a single callback.
//
// Platform class (FreeRTOS), like ServoBank / PlaybackScheduler: exercised by the
// host compile-check, and its pure result-ring logic is unit-tested separately.
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "../../core/configuration/Profile.h"
#include "../../core/util/CommandResultRing.h"

namespace gmb {

enum class CmdType : uint8_t { Panic, Reset, ActivateProfile, TestNote, TestServo, Jog, MoveTo };

struct AppCommand {
    CmdType type;
    uint32_t id = 0;             // for result tracking (GET /api/commands)
    Profile* profile = nullptr;  // owned by the command (ActivateProfile)
    uint8_t channel = 0, note = 0, velocity = 0;
    uint16_t durationMs = 0;
    // TestNote: selection CC values to emit before the Note On, or -1 for none.
    int16_t ccStringValue = -1;
    int16_t ccFretValue = -1;
    int16_t servoIndex = -1;
    bool servoActive = false;
    int16_t axisIndex = -1;      // Jog/MoveTo: which axis
    float jogDeltaMm = 0.0f;     // Jog: signed distance (mm)
    float targetMm = 0.0f;       // MoveTo: absolute position from the HOME zero
};

// What a drained command's handler reports (audit 7). `Deferred` marks a command
// whose real outcome arrives LATER — a profile activation spans park -> swap ->
// re-home -> Ready over many loop passes — and the ring shows it as "running" until
// the owner calls finish() with the final state. Reporting such a command
// "succeeded" the moment it STARTED is how a client concludes the machine is ready
// while it is still parking.
enum class CmdOutcome : uint8_t { Refused = 0, Succeeded = 1, Deferred = 2 };

class CommandDispatcher {
public:
    // Handler runs one command on the main loop and reports its outcome.
    using Handler = std::function<CmdOutcome(const AppCommand&)>;

    void begin(int queueLen) {
        queue_ = xQueueCreate(queueLen, sizeof(AppCommand*));
        capacity_ = queueLen > 0 ? static_cast<uint32_t>(queueLen) : 0;
        resultMutex_ = xSemaphoreCreateMutex();
    }

    // ---- two-phase enqueue -------------------------------------------------
    //
    // Some callers cannot afford "the command is now runnable" and "the change is
    // now on flash" to happen in either order. Publishing a profile is the case:
    //
    //   persist then enqueue   a full queue returns 503 while the flash already
    //                          holds the new configuration — the request failed
    //                          and the next boot changed anyway
    //   enqueue then persist   a flash failure leaves the machine running a
    //                          configuration it will not come back as
    //
    // Splitting the write at its commit point fixed the first half. This splits
    // the queue the same way, so the last window closes too: reserve() claims the
    // capacity and the id but leaves the command invisible to loop(), the caller
    // does its own commit, and only publish() makes it runnable. cancel() gives
    // the slot back. The outcome is always PERSISTED AND ACCEPTED, or NEITHER.
    //
    // Reserved capacity is counted so a later publish() cannot fail for want of
    // room: the accounting is deliberately conservative (a reservation and the
    // message it becomes are both counted for an instant during publish), which
    // can refuse one command early but never over-admits.
    uint32_t reserve() {
        if (!queue_ || capacity_ == 0) return 0;
        uint32_t held = reserved_.fetch_add(1) + 1;
        if (held + depth() > capacity_) {
            reserved_.fetch_sub(1);
            return 0;
        }
        return nextId_.fetch_add(1);
    }

    // Make a reserved command runnable, under the id reserve() handed out. Takes
    // ownership of `in.profile` exactly as enqueue() does.
    bool publish(uint32_t id, const AppCommand& in) {
        if (!queue_ || id == 0) return false;
        AppCommand c = in;
        c.id = id;
        AppCommand* h = new AppCommand(c);
        if (xQueueSend(queue_, &h, 0) != pdTRUE) {
            delete h->profile;  // transfer failed: don't leak the owned profile
            delete h;
            reserved_.fetch_sub(1);
            return false;
        }
        setResult(id, CommandResultRing::Queued);
        reserved_.fetch_sub(1);  // after the send: never under-count in the window
        return true;
    }

    // Release a reservation that will never be published. The id is simply retired;
    // it was never handed to a client, so there is no outcome to record.
    void cancel(uint32_t id) {
        if (id == 0) return;
        reserved_.fetch_sub(1);
    }

    // Reservations outstanding (diagnostics / tests).
    uint32_t reserved() const { return reserved_.load(); }

    // Enqueue a copy of `in` (web task). Assigns an id, records it as "queued" and
    // returns the id — 0 if the queue is full, so the caller reports back-pressure
    // instead of silently dropping the request. The owned copy is freed when the
    // command is drained or purged.
    //
    // Reserve-then-publish, so the capacity accounting is the SAME for every route:
    // a direct enqueue that ignored reservations could take the last slot out from
    // under a publish transaction that had already committed to flash.
    uint32_t enqueue(const AppCommand& in) {
        uint32_t id = reserve();
        if (id == 0) return 0;
        if (!publish(id, in)) return 0;
        return id;
    }

    // Drain a BOUNDED number of queued commands (main loop) so a long burst — many
    // profile activations, say — can never starve the E-stop / panic checks that run
    // each pass. The rest wait for the next tick. Returns the number handled.
    int drain(int maxPerTick, const Handler& handler) {
        if (!queue_) return 0;
        AppCommand* c = nullptr;
        int n = 0;
        for (; n < maxPerTick && xQueueReceive(queue_, &c, 0) == pdTRUE; ++n) {
            CmdOutcome out = handler(*c);
            setResult(c->id, out == CmdOutcome::Succeeded ? CommandResultRing::Succeeded
                           : out == CmdOutcome::Deferred  ? CommandResultRing::Running
                                                          : CommandResultRing::Refused);
            delete c->profile;  // owned copy (null for non-profile commands)
            delete c;
        }
        return n;
    }

    // Record the FINAL state of a previously Deferred command (Succeeded / Failed /
    // Cancelled), from the loop that owns its continuation.
    void finish(uint32_t id, uint8_t state) { setResult(id, state); }

    // Discard every queued command without executing it (used after a panic so a
    // stale profile activation / test can't fire once the STOP has latched). Each
    // dropped command is marked CANCELLED so a client polling it stops at once
    // instead of waiting out a "queued" ghost to its timeout (audit 6).
    void purge() {
        if (!queue_) return;
        AppCommand* c = nullptr;
        while (xQueueReceive(queue_, &c, 0) == pdTRUE) {
            setResult(c->id, CommandResultRing::Cancelled);
            delete c->profile;
            delete c;
        }
    }

    // Live queue depth, for the diagnostics high-water tracking.
    // uxQueueMessagesWaiting is lock-free and safe from the AsyncTCP task.
    uint32_t depth() const {
        return queue_ ? uxQueueMessagesWaiting(queue_) : 0;
    }

    // "queued" / "running" / "succeeded" / "refused" / "cancelled" / "failed" /
    // "unknown" for a command id.
    std::string commandState(uint32_t id) {
        if (resultMutex_) xSemaphoreTake(resultMutex_, portMAX_DELAY);
        std::string s = results_.stateStr(id);
        if (resultMutex_) xSemaphoreGive(resultMutex_);
        return s;
    }

private:
    void setResult(uint32_t id, uint8_t state) {
        if (id == 0) return;
        if (resultMutex_) xSemaphoreTake(resultMutex_, portMAX_DELAY);
        results_.set(id, state);
        if (resultMutex_) xSemaphoreGive(resultMutex_);
    }

    QueueHandle_t queue_ = nullptr;
    SemaphoreHandle_t resultMutex_ = nullptr;
    std::atomic<uint32_t> nextId_{1};
    uint32_t capacity_ = 0;
    std::atomic<uint32_t> reserved_{0};
    CommandResultRing results_;
};

}  // namespace gmb
