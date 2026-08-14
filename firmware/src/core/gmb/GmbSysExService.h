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

#include <string>

#include "Capabilities.h"
#include "GmbDescriptor.h"
#include "GmbSysEx.h"

namespace gmb {

struct Profile;

class GmbSysExService {
public:
    // Handshake flags advertised to GMB: bit0 = HTTP descriptor available
    // (GET /gmb/descriptor.json), bit1 = push notifications (block 0x11 on change).
    static constexpr uint8_t kHandshakeFlags = 0x03;

    void setSnapshot(const CapabilitySnapshot& s) {
        snapshot_ = s;
        descriptorJson_ = GmbDescriptor::toJson(snapshot_);
    }
    const CapabilitySnapshot& snapshot() const { return snapshot_; }

    // The GMB v2 descriptor JSON for the current snapshot (served over block 0x10
    // and over HTTP). Rebuilt on every setSnapshot()/rebuild().
    const std::string& descriptorJson() const { return descriptorJson_; }

    // Rebuild the snapshot from the active profile (call after an atomic save).
    void rebuild(const Profile& p, int polyphonyOverride = -1);

    void setUseV2(bool v) { useV2_ = v; }
    bool useV2() const { return useV2_; }

    // Set a stable per-device identity (e.g. derived from the ESP32 MAC) so two
    // instruments on the same network are distinguishable. Applied on every
    // rebuild(). A non-zero id overrides the profile-derived default.
    void setDeviceId(const uint8_t id[5]) {
        for (int i = 0; i < 5; ++i) deviceId_[i] = id[i];
        hasDeviceId_ = true;
        for (int i = 0; i < 5; ++i) snapshot_.identity.deviceId[i] = id[i];
    }

    // Handle one complete incoming SysEx message. Returns the response bytes, or
    // an empty vector when nothing should be sent (unknown block, malformed,
    // rate-limited, invalid channel).
    std::vector<uint8_t> handleMessage(const uint8_t* data, size_t len,
                                       uint32_t nowMs);

    // Spontaneous capabilities-changed notification (GMB v2 block 0x11).
    std::vector<uint8_t> notification(uint8_t flags) const {
        return GmbSysEx::encodeChangeNotification(snapshot_, flags);
    }

    uint32_t lastResponseMs() const { return lastResponseMs_; }

private:
    CapabilitySnapshot snapshot_;
    std::string descriptorJson_;
    bool useV2_ = false;
    bool hasDeviceId_ = false;
    uint8_t deviceId_[5] = {0, 0, 0, 0, 0};
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
