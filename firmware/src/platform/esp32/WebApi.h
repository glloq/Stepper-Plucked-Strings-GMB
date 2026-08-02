// Local web server: serves the static UI from LittleFS and the REST + WebSocket
// API the interface uses (cahier des charges §9, §19; docs/WEB_INTERFACE.md).
#pragma once

#include <functional>

#include "../../core/configuration/Profile.h"
#include "../../core/gmb/GmbSysExService.h"
#include "../../core/instrument/InstrumentController.h"
#include "Net.h"
#include "ServoBank.h"
#include "StepperBank.h"

#include "../../core/safety/SafetyManager.h"

#if defined(ARDUINO)
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#endif

namespace gmb {

class ProfileStorage;

struct WebContext {
    Profile* profile = nullptr;
    InstrumentController* instrument = nullptr;
    GmbSysExService* sysex = nullptr;
    StepperBank* steppers = nullptr;
    ServoBank* servos = nullptr;
    Net* net = nullptr;
    SafetyManager* safety = nullptr;
    ProfileStorage* storage = nullptr;
    std::function<void()> onPanic;
    std::function<bool()> onReset;                          // recover from panic/E-stop
    std::function<std::string()> appState;                  // "boot"/"homing"/"ready"
    std::function<int()> readyStrings;                      // axes homed & not faulted
    std::function<bool(const Profile&)> onActivateProfile;  // validate + enqueue apply
    std::function<bool(uint8_t, uint8_t, uint8_t, uint16_t)> onTestNote;  // ch,note,vel,ms
    std::function<bool(int, bool)> onTestServo;  // enqueue a servo pulse (index, active)
    // Guard shared state during read-only handlers so a reload in loop() is never
    // observed half-applied. Both may be null (host build / no locking).
    std::function<void()> lockState;
    std::function<void()> unlockState;
    // Only the flagged passwords are written (empty fields are left unchanged).
    std::function<void(bool, const std::string&, bool, const std::string&)> onSetWifi;
    // Returns true if the supplied token authorises a write (or if no admin
    // token has been configured yet — first-run bootstrap).
    std::function<bool(const std::string&)> checkToken;
    std::function<void(const std::string&)> onSetAdminToken;
    // True once an admin token is configured (surfaced so the UI can warn).
    std::function<bool()> authConfigured;
};

class WebApi {
public:
    void begin(const WebContext& ctx, uint16_t port = 80);
    // Rebuild the cached status DTO from live state. MUST be called from loop()
    // (the state owner) only. GET /api/status and the WS broadcast then serve this
    // immutable copy, so the async web task never reads the live vectors.
    void refreshStatus();
    void broadcastStatus();  // push the cached status snapshot over the WebSocket
#if defined(ARDUINO)
    void broadcastMidi(const MidiEvent& e);  // push a MIDI event over /ws/midi
#else
    void broadcastMidi(const MidiEvent&) {}
#endif

private:
    WebContext ctx_;
#if defined(ARDUINO)
    // AsyncWebServer is non-copyable, so it is allocated in begin() to honour the
    // chosen port (it lives for the whole program).
    AsyncWebServer* server_ = nullptr;
    AsyncWebSocket statusWs_{"/ws/status"};
    AsyncWebSocket midiWs_{"/ws/midi"};
    void registerRoutes();
    void fillStatus(JsonDocument& doc);
    bool authOk(AsyncWebServerRequest* req);  // token gate for write routes
    // Cached, serialized status DTO produced by loop() via refreshStatus(); read
    // by the async web task under the state lock so it never touches live state.
    std::string cachedStatus_ = "{}";
#endif
};

}  // namespace gmb
