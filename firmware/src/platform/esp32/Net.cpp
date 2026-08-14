#include "Net.h"

#include <algorithm>

#if defined(ARDUINO)
#include <ESPmDNS.h>
#include <WiFi.h>
#endif

namespace gmb {

namespace {
constexpr uint16_t kDnsPort = 53;
}  // namespace

bool Net::begin(const NetworkConfig& cfg, const std::string& stationPassword,
                const std::string& apPassword) {
    cfg_ = cfg;
    password_ = stationPassword;
    apPassword_ = apPassword;
    // A (re)begin is a fresh start: clear the failure history and any forced-AP
    // latch so newly-stored settings get their full three attempts + fallback.
    failures_ = 0;
    apIsFallback_ = false;
    apForced_ = false;
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
    // Never bring the AP up with an empty SSID (a hand-edited/imported profile could
    // carry one — softAP("") just fails): fall back to the project default so the
    // hotspot always exists.
    const char* ssid =
        cfg_.apSsid.empty() ? "Servo-Plucked-Strings-GMB" : cfg_.apSsid.c_str();
    WiFi.mode(WIFI_AP);
    delay(100);  // let the AP netif start before softAP() (classic-ESP32 flakiness)
    // WPA2 when a password (>= 8 chars) is set, otherwise an open AP. Honour the
    // softAP() return: a failed AP must NOT be reported as connected.
    bool ok = (apPassword_.size() >= 8) ? WiFi.softAP(ssid, apPassword_.c_str())
                                        : WiFi.softAP(ssid);
    if (ok) {
        ip_ = WiFi.softAPIP().toString().c_str();
        apActive_ = true;
        connected_ = true;
        startCaptivePortal();  // any joined client is taken straight to the page
        Serial.printf("[net] hotspot \"%s\" up (%s) — http://%s/\n", ssid,
                      apPassword_.size() >= 8 ? "WPA2" : "open", ip_.c_str());
    } else {
        ip_ = "";
        apActive_ = false;
        connected_ = false;  // AP creation failed — not usable
        Serial.printf("[net] softAP(\"%s\") FAILED — retrying shortly\n", ssid);
    }
#else
    ip_ = "192.168.4.1";
    apActive_ = true;
    connected_ = true;
#endif
    connecting_ = false;
}

void Net::startCaptivePortal() {
#if defined(ARDUINO)
    // Resolve every DNS query to the AP IP so any URL a joined phone/laptop opens
    // (and every OS "is there internet?" probe) lands on this device; the web
    // server then redirects them to the config page (see WebApi captive routes).
    dns_.start(kDnsPort, "*", WiFi.softAPIP());
    dnsActive_ = true;
#endif
}

void Net::stopCaptivePortal() {
#if defined(ARDUINO)
    if (dnsActive_) dns_.stop();
#endif
    dnsActive_ = false;
}

void Net::forceAccessPoint() {
#if defined(ARDUINO)
    if (connecting_ || (!apActive_ && WiFi.status() == WL_CONNECTED)) WiFi.disconnect();
#endif
    apForced_ = true;          // stay in AP; tick() must not retry the station
    startAccessPoint(false);   // brings up softAP + captive portal
}

void Net::beginStationAttempt(uint32_t nowMs) {
    attemptStartMs_ = nowMs;
    connecting_ = true;
    connected_ = false;
    // Recovery attempt FROM the fallback hotspot: keep the rescue AP (and its
    // captive portal) alive in AP+STA while the station retries — the old
    // WIFI_STA switch made the instrument unreachable for up to ~3 x 8 s on every
    // periodic retry while the router stayed down (audit 4 P2.1). A primary
    // station attempt (boot / fresh settings, no AP up) behaves as before.
    bool keepRescueAp = apActive_ && apIsFallback_;
    if (!keepRescueAp) {
        apActive_ = false;
        stopCaptivePortal();  // leaving AP: drop the wildcard DNS
    }
#if defined(ARDUINO)
    WiFi.mode(keepRescueAp ? WIFI_AP_STA : WIFI_STA);
    WiFi.setHostname(cfg_.hostname.c_str());
    WiFi.begin(cfg_.ssid.c_str(), password_.c_str());  // returns immediately
    Serial.printf("[net] joining \"%s\" as \"%s\"%s...\n", cfg_.ssid.c_str(),
                  cfg_.hostname.c_str(),
                  keepRescueAp ? " (rescue hotspot kept up)" : "");
#endif
}

bool Net::pollStation(uint32_t nowMs) {
#if defined(ARDUINO)
    if (WiFi.status() == WL_CONNECTED) {
        // Station is up: if the rescue hotspot was kept alive during the retry
        // (AP+STA), it has done its job — shut it down cleanly now.
        if (apActive_) {
            WiFi.softAPdisconnect(true);
            stopCaptivePortal();
            apActive_ = false;
            apIsFallback_ = false;
            WiFi.mode(WIFI_STA);
        }
        ip_ = WiFi.localIP().toString().c_str();
        connected_ = true;
        connecting_ = false;
        failures_ = 0;
        if (!cfg_.hostname.empty()) MDNS.begin(cfg_.hostname.c_str());
        Serial.printf("[net] station connected — http://%s/ (http://%s.local/)\n",
                      ip_.c_str(), cfg_.hostname.c_str());
        return true;
    }
    // Still trying; give up on this attempt after 8 s (no blocking wait).
    if (nowMs - attemptStartMs_ > 8000) {
        connecting_ = false;
        Serial.printf("[net] station attempt timed out (%d/3)\n", failures_ + 1);
        if (++failures_ >= 3) {
            // Fall back to AP (spec §8.1). If the rescue AP was kept up during
            // this retry it is already serving — just restart the retry clock.
            if (!apActive_) startAccessPoint(true);
            lastStationRetryMs_ = nowMs;
        } else if (apActive_ && apIsFallback_) {
            // Recovery from the rescue AP: run the "3 attempts" BACK-TO-BACK
            // (the AP stays up in AP+STA throughout), then pause for the 60 s
            // retry clock — so "attempt X/3" means what it says (audit 5).
            beginStationAttempt(nowMs);
        }
    }
    return false;
#else
    (void)nowMs;
    return false;
#endif
}

bool Net::startScan() {
    if (scanning_) return false;
#if defined(ARDUINO)
    // Survey from the station interface. While the AP is up, switch to AP+STA so
    // the hotspot (and the browser session riding on it) stays alive during the
    // scan; a later station attempt / AP restart resets the mode as usual.
    if (apActive_) WiFi.mode(WIFI_AP_STA);
    WiFi.scanNetworks(true /*async*/, false /*skip hidden*/);
#endif
    scanning_ = true;
    return true;
}

void Net::pollScan() {
    if (!scanning_) return;
#if defined(ARDUINO)
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;
    scanResults_.clear();
    for (int i = 0; i < n; ++i) {
        ScanResult r;
        r.ssid = WiFi.SSID(i).c_str();
        r.rssi = WiFi.RSSI(i);
        r.secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        r.channel = WiFi.channel(i);
        if (r.ssid.empty()) continue;  // unnamed/hidden entries are unusable here
        // Deduplicate by SSID — a mesh shows one row, the strongest signal wins.
        bool dup = false;
        for (auto& e : scanResults_) {
            if (e.ssid == r.ssid) {
                dup = true;
                if (r.rssi > e.rssi) e = r;
                break;
            }
        }
        if (!dup) scanResults_.push_back(r);
    }
    std::sort(scanResults_.begin(), scanResults_.end(),
              [](const ScanResult& a, const ScanResult& b) { return a.rssi > b.rssi; });
    WiFi.scanDelete();
#else
    scanResults_.clear();
#endif
    scanning_ = false;
    ++scanGeneration_;
}

void Net::tick(uint32_t nowMs) {
    pollScan();  // harvest a finished background scan in any mode
#if defined(ARDUINO)
    if (apActive_) {
        if (dnsActive_) dns_.processNextRequest();  // captive portal
        // A recovery attempt in AP+STA (rescue hotspot kept up): keep polling the
        // station; success shuts the AP down inside pollStation().
        if (connecting_) {
            pollStation(nowMs);
            return;
        }
        // If the AP is only a FALLBACK (a station was configured but unreachable),
        // periodically retry the station so a transient router outage recovers.
        // A primary AP-mode profile — or a BOOT-button-forced hotspot — stays in AP
        // and never retries. The rescue AP stays up during the attempt (AP+STA).
        if (!apForced_ && apIsFallback_ && cfg_.mode == NetworkMode::Station &&
            !cfg_.ssid.empty() && nowMs - lastStationRetryMs_ >= 60000) {
            lastStationRetryMs_ = nowMs;
            failures_ = 0;
            beginStationAttempt(nowMs);
        }
        return;
    }

    if (connecting_) {
        pollStation(nowMs);
        return;
    }

    // A BOOT-button / web-forced hotspot must NEVER fall back to the station — even
    // if softAP failed to come up (transient heap/radio). Keep retrying the AP
    // instead of attempting the (possibly wrong) station config. Throttled so a hard
    // softAP failure can't hammer the radio every loop. (lastStationRetryMs_ doubles
    // as the retry clock here; the station-retry branch above is gated off by
    // apForced_, so there is no conflict.)
    if (apForced_) {
        if (nowMs - lastStationRetryMs_ >= 2000) {
            lastStationRetryMs_ = nowMs;
            startAccessPoint(false);
        }
        return;
    }

    // AP-primary profile (AccessPoint mode, or Station with no SSID configured):
    // reaching here means the softAP failed to come up. Keep retrying the AP
    // (throttled) — NEVER attempt the station: WiFi.begin() with an empty SSID just
    // thrashes the radio and leaves the instrument unreachable. (As in the apForced_
    // branch above, lastStationRetryMs_ doubles as the AP retry clock; the station
    // branches below are only reachable with a real station config.)
    if (cfg_.mode != NetworkMode::Station || cfg_.ssid.empty()) {
        if (nowMs - lastStationRetryMs_ >= 2000) {
            lastStationRetryMs_ = nowMs;
            startAccessPoint(false);
        }
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
