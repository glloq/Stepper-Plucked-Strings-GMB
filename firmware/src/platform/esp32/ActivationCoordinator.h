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
// ONE transaction at a time, deliberately. Two overlapping publishes would share the
// single /active.json.tmp: B's prepare would overwrite A's staged bytes and A's
// commit would rename B's file into place — activating A while storing B, which is
// the exact failure this whole model exists to remove. Rather than leave that
// impossible by convention (nothing stops two AsyncTCP callbacks from overlapping)
// or mint a temp file per transaction on a small flash, a second publish is refused
// while one is in flight, and the caller reports it as back-pressure.
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
#include "CommandDispatcher.h"

namespace gmb {

class ActivationCoordinator {
public:
    // Resolve the profile that will actually run. `keepDeviceConfig` selects
    // between "take this draft whole" and "keep this machine's device half"; the
    // owner supplies it because only the owner may read the running profile, and
    // only under its own state lock.
    using Merge = std::function<Profile(const Profile& in, bool keepDeviceConfig)>;

    void begin(CommandDispatcher* dispatcher, Merge merge) {
        dispatcher_ = dispatcher;
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
    uint32_t reserve(const Profile& in, bool keepDeviceConfig, Profile& mergedOut) {
        if (!dispatcher_) return 0;
        Profile target = merge_ ? merge_(in, keepDeviceConfig) : in;
        // Validate the MERGED profile: the instrument half has to fit THIS device's
        // pins, and that combination is what will run. Pure, so it is safe here on
        // the web task and rejects an impossible profile before anything is claimed.
        if (!ProfileValidator::isActivatable(target)) return 0;

        Guard lock(mutex_);
        if (token_ != 0) return 0;             // one transaction at a time
        uint32_t token = dispatcher_->reserve();
        if (token == 0) return 0;              // no queue capacity
        token_ = token;
        pending_ = target;
        mergedOut = target;
        return token;
    }

    // Make the reserved activation runnable. Called only once the write is committed.
    bool publish(uint32_t token) {
        Guard lock(mutex_);
        if (!dispatcher_ || token == 0 || token_ != token) return false;
        AppCommand c{CmdType::ActivateProfile};
        c.profile = new Profile(pending_);          // ownership passes to the command
        bool ok = dispatcher_->publish(token, c);   // frees it if the send fails
        clearLocked();
        return ok;
    }

    // Release a reservation that will never be published.
    void cancel(uint32_t token) {
        Guard lock(mutex_);
        if (!dispatcher_ || token == 0 || token_ != token) return;
        dispatcher_->cancel(token);
        clearLocked();
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
    Merge merge_;
    SemaphoreHandle_t mutex_ = nullptr;
    uint32_t token_ = 0;
    Profile pending_;
};

}  // namespace gmb
