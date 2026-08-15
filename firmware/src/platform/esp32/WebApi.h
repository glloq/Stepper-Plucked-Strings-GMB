// Local web server: serves the static UI from LittleFS and the REST + WebSocket
// API the interface uses (spec §9, §19; docs/WEB_INTERFACE.md).
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
    // The enqueue callbacks return the assigned command id (0 = queue full), so
    // the 202 response can carry it and GET /api/commands can report the outcome.
    std::function<uint32_t()> onReset;                     // recover from panic/E-stop
    std::function<std::string()> appState;                 // "boot"/"homing"/"ready"
    std::function<int()> readyStrings;                     // axes homed & not faulted
    // ---- the publish transaction -------------------------------------------
    //
    // Activating a profile and persisting it must both happen or neither must, so
    // the enqueue is split at the same point the storage write already was. See
    // publishProfile() below, and ActivationCoordinator.h.
    //
    // reserve : validate, merge, and claim a queue slot WITHOUT making the command
    //           runnable. Returns the future command id, or 0 — invalid profile, no
    //           queue capacity, or another publish already in flight. `mergedOut`
    //           receives the exact profile that will run, which is the profile the
    //           caller must write: deriving those bytes separately means two merges
    //           that have to agree.
    // publish : make it runnable. Called only after the commit succeeded.
    // cancel  : drop the reservation. Nothing changes anywhere.
    //
    // `keepDeviceConfig` distinguishes the two operations that reach here, which
    // are NOT the same thing:
    //   false — PUT /api/profile: publish the draft the user just edited FOR THIS
    //           machine. Everything in it is intended, pins and network included.
    //   true  — POST /api/profiles/load: load a stored INSTRUMENT onto this
    //           machine. The device half (board, pins, network, E-stop polarity,
    //           fitted hardware) belongs to the machine and must survive.
    std::function<uint32_t(const Profile&, bool, Profile&)> onReserveActivation;
    std::function<bool(uint32_t)> onPublishActivation;
    std::function<void(uint32_t)> onCancelActivation;
    // ch, note, vel, ms, then the two optional SELECTION CC values (-1 = none).
    // They are emitted before the Note On through the same path a controller's
    // CCs take, so the test really exercises the string/fret selector.
    std::function<uint32_t(uint8_t, uint8_t, uint8_t, uint16_t, int, int)> onTestNote;
    std::function<uint32_t(int, bool)> onTestServo;  // enqueue a servo pulse (index, active)
    std::function<uint32_t(int, double)> onJog;      // enqueue an axis jog (axis, deltaMm)
    // Absolute move (axis, positionMm from the homing zero). Fret calibration
    // needs a real target: summing relative jogs accumulates every rounding error
    // into the position the operator then records as ground truth.
    std::function<uint32_t(int, double)> onMoveTo;
    std::function<std::string(uint32_t)> commandState;
    std::function<std::string()> diagnosticsJson;  // GET /api/diagnostics body (P2.19)
    // Switch to the access point on demand (POST /api/hotspot) — the web twin of
    // the BOOT-button hotspot, for when the station link is unreachable.
    std::function<void()> onStartHotspot;
    // Asynchronous Wi-Fi survey for the network picker (GET /api/wifi/scan).
    std::function<std::string()> wifiScanJson;
    std::function<void()> onWifiScanStart;
    // Live UDP source posture (audit P1.11) so the Settings UI shows the real state.
    std::function<std::string()> midiSourcePolicy;
    std::function<bool()> midiSourceLocked;  // queued/succeeded/refused/unknown
    // Live state of every MIDI transport. `/api/status` used to answer a hard-coded
    // "wifiUdp", which was a lie the moment a second transport existed: with a DIN
    // cable plugged in and the Wi-Fi link down, the page still claimed Wi-Fi UDP.
    // The firmware knows which transports are bound and which one last delivered a
    // byte, so it reports that instead of a constant.
    struct MidiTransportState {
        std::string name;          // "wifiUdp" | "usb" | "din"
        std::string label;         // human-readable, for the UI
        bool bound = false;        // has real hardware behind it (pin / socket / stack)
        std::string detail;        // why it is or is not bound (port, GPIO, "no MIDI_RX pin")
        uint32_t events = 0;       // messages decoded since boot
        uint32_t lastEventMs = 0;  // millis() of the most recent decoded message (0 = never)
    };
    std::function<std::vector<MidiTransportState>()> midiTransports;
    // Which transport most recently delivered a message, and when. This is the
    // answer to "what is playing this instrument right now"; `events` is a lifetime
    // total and answers a different question. Picking the highest total was wrong
    // for the case that matters: plug a DIN cable into a machine that has been on
    // Wi-Fi all day and it stays "wifiUdp" no matter what you play.
    std::function<std::string()> lastMidiSource;
    std::function<uint32_t()> lastMidiEventMs;
    // POST /api/midi/source. policy: -1 leave unchanged, 0 open, 1 lockToFirst,
    // 2 disabled; `unlock` forgets the currently locked sender. Returns false when
    // the setting could not be persisted (the caller then reports a real failure
    // rather than claiming success).
    std::function<bool(int, bool)> onSetMidiSource;
    std::function<bool()> onFormatStorage;       // deliberate LittleFS reformat
    // Guard shared state during read-only handlers so a reload in loop() is never
    // observed half-applied. Both may be null (host build / no locking).
    std::function<void()> lockState;
    std::function<void()> unlockState;
    // Distinct lock for LittleFS operations. loop() never takes it, so a long
    // flash write from the web task cannot stall the safety loop.
    std::function<void()> lockStorage;
    std::function<void()> unlockStorage;
    // POST /api/wifi. Everything is optional and only the flagged fields are
    // written, so the UI can send a password without touching the link config and
    // vice versa. Clearing is explicit: an empty password field means "leave the
    // stored secret alone", which is why erasing one needs its own flag.
    struct WifiRequest {
        bool hasStationPassword = false;
        std::string stationPassword;
        bool hasApPassword = false;
        std::string apPassword;
        bool clearStationPassword = false;   // really erase the stored secret
        bool clearApPassword = false;        // (an OPEN access point)
        bool hasNetwork = false;             // mode/ssid/apSsid/hostname supplied
        NetworkConfig network;
        bool apply = false;                  // reconfigure the link NOW, not at boot
    };
    // Returns the note echoed to the caller, so the UI can state what really
    // happened ("applied now" vs "stored; reboot to apply") instead of guessing.
    std::function<std::string(const WifiRequest&)> onSetWifi;
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

    // What one publish transaction did. `error` is null on success.
    //
    // The whole point is that only two of the possible field combinations can ever
    // be observed: everything true, or `accepted` and `persisted` both false. There
    // is no longer a state where the machine runs one configuration and boots
    // another, so callers no longer have to warn about one.
    struct PublishResult {
        bool accepted = false;    // the activation is queued for loop()
        bool persisted = false;   // /active.json now holds it
        uint32_t commandId = 0;
        const char* error = nullptr;
        int httpStatus = 202;
    };

    // Validate + merge + reserve, write, commit, publish — in that order, with
    // every failure path releasing the reservation. Shared by PUT /api/profile and
    // POST /api/profiles/load, which differ only in `keepDeviceConfig` and in what
    // they say afterwards; they used to carry a copy of this sequence each.
    //
    // Public because the ordering IS the guarantee, and a guarantee nothing can
    // call is a guarantee nothing can test — runtimecheck drives this directly,
    // off-Arduino, with a storage that fails where it is told to.
    PublishResult publishProfile(const Profile& p, bool keepDeviceConfig);

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
    std::string captivePortalUrl() const;     // "http://<ap-ip>/" for redirects
    // Cached, serialized status DTO produced by loop() via refreshStatus(); read
    // by the async web task under the state lock so it never touches live state.
    std::string cachedStatus_ = "{}";
#endif
};

}  // namespace gmb
