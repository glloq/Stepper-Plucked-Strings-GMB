// Wi-Fi management: AP for first setup, station for normal use, automatic
// fallback to AP after repeated failures (spec §8.1).
#pragma once

#include <cstdint>
#include <string>

#include "../../core/configuration/Profile.h"

namespace gmb {

class Net {
public:
    // Passwords are supplied separately from the Profile so they are never
    // stored in exportable config (spec §20). An empty apPassword
    // leaves the access point open.
    bool begin(const NetworkConfig& cfg, const std::string& stationPassword,
               const std::string& apPassword = "");

    // Poll connection state; returns to AP mode after repeated station failures.
    void tick(uint32_t nowMs);

    bool connected() const { return connected_; }
    bool accessPointActive() const { return apActive_; }
    // True only when the AP is up AND protected by a WPA2 password (>= 8 chars).
    bool accessPointSecured() const { return apActive_ && apPassword_.size() >= 8; }
    std::string ipAddress() const { return ip_; }
    std::string mode() const { return apActive_ ? "accessPoint" : "station"; }

private:
    NetworkConfig cfg_;
    std::string password_;
    std::string apPassword_;
    bool connected_ = false;
    bool apActive_ = false;
    bool connecting_ = false;
    bool apIsFallback_ = false;  // AP entered because station failed (retry later)
    std::string ip_;
    int failures_ = 0;
    uint32_t attemptStartMs_ = 0;
    uint32_t lastStationRetryMs_ = 0;

    void startAccessPoint(bool fallback = false);
    void beginStationAttempt(uint32_t nowMs);  // non-blocking: kicks off WiFi.begin
    bool pollStation(uint32_t nowMs);          // returns true once connected
};

}  // namespace gmb
