#include "Net.h"

#if defined(ARDUINO)
#include <ESPmDNS.h>
#include <WiFi.h>
#endif

namespace gmb {

bool Net::begin(const NetworkConfig& cfg, const std::string& stationPassword,
                const std::string& apPassword) {
    cfg_ = cfg;
    password_ = stationPassword;
    apPassword_ = apPassword;
    if (cfg_.mode == NetworkMode::Station && !cfg_.ssid.empty()) {
#if defined(ARDUINO)
        beginStationAttempt(millis());
#else
        beginStationAttempt(0);
#endif
        return true;  // connection proceeds asynchronously in tick()
    }
    startAccessPoint();
    return true;
}

void Net::startAccessPoint(bool fallback) {
    apIsFallback_ = fallback;
#if defined(ARDUINO)
    WiFi.mode(WIFI_AP);
    // WPA2 when a password (>= 8 chars) is set, otherwise an open AP. Honour the
    // softAP() return: a failed AP must NOT be reported as connected.
    bool ok = (apPassword_.size() >= 8)
                  ? WiFi.softAP(cfg_.apSsid.c_str(), apPassword_.c_str())
                  : WiFi.softAP(cfg_.apSsid.c_str());
    if (ok) {
        ip_ = WiFi.softAPIP().toString().c_str();
        apActive_ = true;
        connected_ = true;
    } else {
        ip_ = "";
        apActive_ = false;
        connected_ = false;  // AP creation failed — not usable
    }
#else
    ip_ = "192.168.4.1";
    apActive_ = true;
    connected_ = true;
#endif
    connecting_ = false;
}

void Net::beginStationAttempt(uint32_t nowMs) {
    attemptStartMs_ = nowMs;
    connecting_ = true;
    connected_ = false;
    apActive_ = false;
#if defined(ARDUINO)
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(cfg_.hostname.c_str());
    WiFi.begin(cfg_.ssid.c_str(), password_.c_str());  // returns immediately
#endif
}

bool Net::pollStation(uint32_t nowMs) {
#if defined(ARDUINO)
    if (WiFi.status() == WL_CONNECTED) {
        ip_ = WiFi.localIP().toString().c_str();
        connected_ = true;
        connecting_ = false;
        failures_ = 0;
        if (!cfg_.hostname.empty()) MDNS.begin(cfg_.hostname.c_str());
        return true;
    }
    // Still trying; give up on this attempt after 8 s (no blocking wait).
    if (nowMs - attemptStartMs_ > 8000) {
        connecting_ = false;
        if (++failures_ >= 3) {
            startAccessPoint(true);  // fall back to AP (spec §8.1)
            lastStationRetryMs_ = nowMs;
        }
    }
    return false;
#else
    (void)nowMs;
    return false;
#endif
}

void Net::tick(uint32_t nowMs) {
#if defined(ARDUINO)
    if (apActive_) {
        // If the AP is only a FALLBACK (a station was configured but unreachable),
        // periodically retry the station so a transient router outage recovers.
        // A primary AP-mode profile stays in AP and never retries.
        if (apIsFallback_ && cfg_.mode == NetworkMode::Station &&
            !cfg_.ssid.empty() && nowMs - lastStationRetryMs_ >= 60000) {
            lastStationRetryMs_ = nowMs;
            failures_ = 0;
            beginStationAttempt(nowMs);  // drops the AP while trying; re-falls back
        }
        return;
    }

    if (connecting_) {
        pollStation(nowMs);
        return;
    }

    if (WiFi.status() == WL_CONNECTED) {
        connected_ = true;
        failures_ = 0;
        return;
    }

    // Lost the link: start a fresh non-blocking attempt.
    connected_ = false;
    beginStationAttempt(nowMs);
#else
    (void)nowMs;
#endif
}

}  // namespace gmb
