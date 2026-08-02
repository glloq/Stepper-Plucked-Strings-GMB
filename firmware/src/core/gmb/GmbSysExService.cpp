#include "GmbSysExService.h"

#include "../configuration/Profile.h"

namespace gmb {

void GmbSysExService::rebuild(const Profile& p, int polyphonyOverride) {
    snapshot_ = buildSnapshot(p, polyphonyOverride);
    // Preserve the stable device identity across rebuilds (buildSnapshot resets
    // it to the profile default).
    if (hasDeviceId_)
        for (int i = 0; i < 5; ++i) snapshot_.identity.deviceId[i] = deviceId_[i];
}

bool GmbSysExService::allow(uint32_t nowMs) {
    if (lastRefillMs_ == 0) lastRefillMs_ = nowMs;
    uint32_t elapsed = nowMs - lastRefillMs_;
    if (elapsed >= kRefillMs) {
        int add = static_cast<int>(elapsed / kRefillMs);
        tokens_ = tokens_ + add > kMaxTokens ? kMaxTokens : tokens_ + add;
        lastRefillMs_ += static_cast<uint32_t>(add) * kRefillMs;
    }
    if (tokens_ <= 0) return false;
    --tokens_;
    return true;
}

std::vector<uint8_t> GmbSysExService::handleMessage(const uint8_t* data, size_t len,
                                                    uint32_t nowMs) {
    if (!GmbSysEx::isWellFormed(data, len)) return {};  // ignore malformed

    SysExRequest req = GmbSysEx::parseRequest(data, len);
    if (!req.valid) return {};  // unknown block / not a request

    // Single-channel instrument: reject requests for a foreign channel.
    if (req.hasChannel && req.channel != snapshot_.capabilities.channel) {
        return {};  // invalid channel (SysEx spec §20)
    }

    // Enforce the rate limit: drop (no response, no work) when out of tokens.
    if (!allow(nowMs)) return {};
    lastResponseMs_ = nowMs;

    return GmbSysEx::respond(req, snapshot_, useV2_);
}

}  // namespace gmb
