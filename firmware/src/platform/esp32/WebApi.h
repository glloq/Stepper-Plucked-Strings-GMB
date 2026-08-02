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
    std::function<bool(const Profile&)> onActivateProfile;  // validate + apply
    std::function<bool(uint8_t, uint8_t, uint8_t, uint16_t)> onTestNote;  // ch,note,vel,ms
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
    void broadcastStatus();  // push live state over the status WebSocket
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
#endif
};

}  // namespace gmb
