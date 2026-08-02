#include "Net.h"

#if defined(ARDUINO)
#include <ESPmDNS.h>
#include <WiFi.h>
#endif

namespace gmb {

bool Net::begin(const NetworkConfig& cfg, const std::string& stationPassword) {
    cfg_ = cfg;
    password_ = stationPassword;
    if (cfg_.mode == NetworkMode::Station && !cfg_.ssid.empty()) {
        if (startStation()) return true;
    }
    startAccessPoint();
    return true;
}

void Net::startAccessPoint() {
#if defined(ARDUINO)
    WiFi.mode(WIFI_AP);
    WiFi.softAP(cfg_.apSsid.c_str());
    ip_ = WiFi.softAPIP().toString().c_str();
#else
    ip_ = "192.168.4.1";
#endif
    apActive_ = true;
    connected_ = true;
}

bool Net::startStation() {
#if defined(ARDUINO)
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(cfg_.hostname.c_str());
    WiFi.begin(cfg_.ssid.c_str(), password_.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
        delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
        ip_ = WiFi.localIP().toString().c_str();
        apActive_ = false;
        connected_ = true;
        if (!cfg_.hostname.empty()) MDNS.begin(cfg_.hostname.c_str());
        return true;
    }
    return false;
#else
    return false;
#endif
}

void Net::tick(uint32_t nowMs) {
#if defined(ARDUINO)
    if (apActive_) return;  // AP mode is stable
    if (WiFi.status() == WL_CONNECTED) {
        connected_ = true;
        failures_ = 0;
        return;
    }
    connected_ = false;
    if (nowMs - lastAttemptMs_ < 5000) return;
    lastAttemptMs_ = nowMs;
    if (!startStation()) {
        if (++failures_ >= 3) startAccessPoint();  // fall back (cahier §8.1)
    }
#else
    (void)nowMs;
#endif
}

}  // namespace gmb
