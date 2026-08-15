// The publish transaction's queue half, extracted from main.cpp.
//
// Publishing a profile must end in exactly one of two states:
//
//     persisted AND accepted for activation      or      neither
//
// The storage half reached that first, by splitting the write at its commit point
// (prepare / commit / discard). The queue half was still a plain enqueue, so this
// interleaving stayed reachable:
//
//     prepare  OK   the new configuration is staged
//     enqueue  OK   loop() can now pick it up  <-- too early
//     commit   FAIL the rename did not happen
//
// and the machine ran profile B with profile A on flash. The response said so, which
// is honest, but "your machine is running something it will not come back as" is not
// an outcome worth reporting rather than preventing.
//
// So the queue is split the same way. reserve() validates, merges, and claims a
// queue slot while leaving the command INVISIBLE to loop(); the caller commits its
// write; publish() then makes the command runnable. cancel() gives the slot back and
// nothing anywhere has changed.
//
// ONE transaction at a time, deliberately — see ActiveSnapshotLock.h. That
// exclusion used to live here, which made it a guard between publishes and nothing
// more: POST /api/wifi writes the SAME snapshot (the link config is in its device
// half) and was never asked to take it. The lock now sits outside both, so every
// writer of /active.json takes the same one.
//
// Platform class (FreeRTOS) like CommandDispatcher, and driven by the real
// runtimecheck harness rather than only compiled.
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdint>
#include <functional>

#include "../../core/configuration/Profile.h"
#include "../../core/configuration/ProfileValidator.h"
#include "ActiveSnapshotLock.h"
#include "CommandDispatcher.h"

namespace gmb {

// Why a reservation failed. Three different answers used to share one `0`, and the
// route reported all three as 503 "try again" — including an instrument that is
// simply incompatible with this machine, which no amount of retrying will fix and
// which is a 422.
enum class ReserveStatus : uint8_t {
    Ok,
    InvalidProfile,  // 422: this instrument cannot run on this device, ever
    QueueFull,       // 503: transient, retry
    Busy,            // 503: another snapshot write is in flight, retry
};

struct ReserveResult {
    uint32_t token = 0;
    ReserveStatus status = ReserveStatus::InvalidProfile;
    bool ok() const { return status == ReserveStatus::Ok && token != 0; }
};

class ActivationCoordinator {
public:
    // Resolve the profile that will actually run. `keepDeviceConfig` selects
    // between "take this draft whole" and "keep this machine's device half"; the
    // owner supplies it because only the owner may read the running profile, and
    // only under its own state lock.
    using Merge = std::function<Profile(const Profile& in, bool keepDeviceConfig)>;

    void begin(CommandDispatcher* dispatcher, ActiveSnapshotLock* snapshotLock,
               Merge merge) {
        dispatcher_ = dispatcher;
        snapshotLock_ = snapshotLock;
        merge_ = std::move(merge);
        mutex_ = xSemaphoreCreateMutex();
    }

    // Claim the transaction and a queue slot. Returns the id the command WILL carry,
    // or 0 — invalid profile, no queue capacity, or another publish in flight, all
    // of which mean the same thing to the caller: nothing changed, try again.
    //
    // `mergedOut` receives the profile that will run, which is therefore the profile
    // the caller must store. Deriving those bytes separately is two merges that have
    // to agree, and they did diverge once already.
    ReserveResult reserve(const Profile& in, bool keepDeviceConfig, Profile& mergedOut) {
        ReserveResult r;
        if (!dispatcher_) { r.status = ReserveStatus::Busy; return r; }
        Profile target = merge_ ? merge_(in, keepDeviceConfig) : in;
        // Validate the MERGED profile: the instrument half has to fit THIS device's
        // pins, and that combination is what will run. Pure, so it is safe here on
        // the web task and rejects an impossible profile before anything is claimed.
        if (!ProfileValidator::isActivatable(target)) {
            r.status = ReserveStatus::InvalidProfile;
            return r;
        }
        // The snapshot lock BEFORE the queue slot: it is the scarcer resource and
        // the one whose loss would corrupt something, so failing to get it should
        // cost nothing.
        if (snapshotLock_ && !snapshotLock_->tryAcquire()) {
            r.status = ReserveStatus::Busy;
            return r;
        }
        Guard lock(mutex_);
        uint32_t token = dispatcher_->reserve();
        if (token == 0) {
            if (snapshotLock_) snapshotLock_->release();
            r.status = ReserveStatus::QueueFull;
            return r;
        }
        token_ = token;
        pending_ = target;
        mergedOut = target;
        r.token = token;
        r.status = ReserveStatus::Ok;
        return r;
    }

    // Make the reserved activation runnable. Called only once the write is committed.
    bool publish(uint32_t token) {
        Guard lock(mutex_);
        if (!dispatcher_ || token == 0 || token_ != token) return false;
        AppCommand c{CmdType::ActivateProfile};
        c.profile = new Profile(pending_);          // ownership passes to the command
        bool ok = dispatcher_->publish(token, c);   // frees it if the send fails
        clearLocked();
        if (snapshotLock_) snapshotLock_->release();
        return ok;
    }

    // Release a reservation that will never be published.
    void cancel(uint32_t token) {
        Guard lock(mutex_);
        if (!dispatcher_ || token == 0 || token_ != token) return;
        dispatcher_->cancel(token);
        clearLocked();
        if (snapshotLock_) snapshotLock_->release();
    }

    // Is a transaction in flight? Diagnostics and tests only — never use it to
    // decide whether to start one, since the answer can change between the test and
    // the act. reserve() makes that decision atomically, which is the point.
    bool busy() {
        Guard lock(mutex_);
        return token_ != 0;
    }

private:
    // Guards the pending slot, and held only for the few instructions that test and
    // set it — NEVER across the caller's flash write. It is the slot's OCCUPANCY
    // that excludes a second transaction, not a held lock: keeping a semaphore from
    // reserve() until publish() would mean a web handler returning early between the
    // two wedges the device until reboot. The invariant lives in the caller instead
    // — every path after a successful reserve() ends in exactly one publish/cancel —
    // which is checkable by reading one function.
    struct Guard {
        SemaphoreHandle_t m;
        explicit Guard(SemaphoreHandle_t s) : m(s) { if (m) xSemaphoreTake(m, portMAX_DELAY); }
        ~Guard() { if (m) xSemaphoreGive(m); }
    };

    void clearLocked() {
        token_ = 0;
        pending_ = Profile{};
    }

    CommandDispatcher* dispatcher_ = nullptr;
    ActiveSnapshotLock* snapshotLock_ = nullptr;
    Merge merge_;
    SemaphoreHandle_t mutex_ = nullptr;
    uint32_t token_ = 0;
    Profile pending_;
};

}  // namespace gmb
