#include "GmbSysExService.h"

#include "../configuration/Profile.h"

namespace gmb {

void GmbSysExService::rebuild(const Profile& p, int polyphonyOverride) {
    snapshot_ = buildSnapshot(p, polyphonyOverride);
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

    // Basic response-rate limit.
    if (lastResponseMs_ != 0 && (nowMs - lastResponseMs_) < minIntervalMs_) {
        // Still answer, but do not update the throttle window aggressively.
    }
    lastResponseMs_ = nowMs;

    return GmbSysEx::respond(req, snapshot_, useV2_);
}

}  // namespace gmb
