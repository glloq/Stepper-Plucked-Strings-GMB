// Transport-independent GMB SysEx service (SysEx spec §19/§21).
//
// Holds one immutable capability snapshot and answers incoming requests from it,
// so a configuration change mid-transmission can never mix two profile versions.
// The transport only hands it complete MIDI SysEx buffers and sends whatever
// bytes it returns.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Capabilities.h"
#include "GmbSysEx.h"

namespace gmb {

struct Profile;

class GmbSysExService {
public:
    void setSnapshot(const CapabilitySnapshot& s) { snapshot_ = s; }
    const CapabilitySnapshot& snapshot() const { return snapshot_; }

    // Rebuild the snapshot from the active profile (call after an atomic save).
    void rebuild(const Profile& p, int polyphonyOverride = -1);

    void setUseV2(bool v) { useV2_ = v; }
    bool useV2() const { return useV2_; }

    // Handle one complete incoming SysEx message. Returns the response bytes, or
    // an empty vector when nothing should be sent (unknown block, malformed,
    // rate-limited, invalid channel).
    std::vector<uint8_t> handleMessage(const uint8_t* data, size_t len,
                                       uint32_t nowMs);

    // Spontaneous capabilities-changed notification (block 8).
    std::vector<uint8_t> notification(uint8_t flags) const {
        return GmbSysEx::encodeNotification(snapshot_, flags);
    }

    uint32_t lastResponseMs() const { return lastResponseMs_; }

private:
    CapabilitySnapshot snapshot_;
    bool useV2_ = false;
    uint32_t lastResponseMs_ = 0;

    // Token-bucket rate limiter (SysEx spec §20): allows a discovery burst but
    // caps sustained request floods so repeated UDP packets cannot force endless
    // responses/allocations.
    static constexpr int kMaxTokens = 16;
    static constexpr uint32_t kRefillMs = 5;  // +1 token every 5 ms (~200/s)
    int tokens_ = kMaxTokens;
    uint32_t lastRefillMs_ = 0;
    bool allow(uint32_t nowMs);
};

}  // namespace gmb
