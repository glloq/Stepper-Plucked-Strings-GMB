#include "WebApi.h"

#include "ProfileStorage.h"
#include "WebAssets.h"
#include "../../core/configuration/DeviceInstrument.h"
#include "../../core/configuration/ProfileValidator.h"

#if defined(ARDUINO)
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <LittleFS.h>
#endif

namespace gmb {

// The two context locks. OUTSIDE the ARDUINO guard because publishProfile() is
// outside it too — the transaction is the piece worth driving on a host, and it
// takes the storage lock like every other route.
namespace {

// RAII guard for the shared-state lock supplied by the main loop. Held only while
// a read-only handler samples g_profile / instrument / steppers / sysex so a
// reload in loop() is never observed half-applied.
struct WebStateLock {
    const WebContext& ctx;
    explicit WebStateLock(const WebContext& c) : ctx(c) { if (ctx.lockState) ctx.lockState(); }
    ~WebStateLock() { if (ctx.unlockState) ctx.unlockState(); }
};

// RAII guard for the LittleFS lock (distinct from the state lock: loop() never
// waits on it, so a flash write here can't stall the safety loop).
struct WebStorageLock {
    const WebContext& ctx;
    explicit WebStorageLock(const WebContext& c) : ctx(c) { if (ctx.lockStorage) ctx.lockStorage(); }
    ~WebStorageLock() { if (ctx.unlockStorage) ctx.unlockStorage(); }
};

}  // namespace

#if defined(ARDUINO)

namespace {

const char* prefName(PinPreference p) {
    switch (p) {
        case PinPreference::Recommended: return "recommended";
        case PinPreference::Caution: return "caution";
        case PinPreference::Reserved: return "reserved";
        default: return "used";
    }
}

void sendJson(AsyncWebServerRequest* req, JsonDocument& doc, int code = 200) {
    String out;
    serializeJson(doc, out);
    req->send(code, "application/json", out);
}

const char* stringStateName(StringState s) {
    switch (s) {
        case StringState::Disabled:        return "disabled";
        case StringState::Homing:          return "homing";
        case StringState::Idle:            return "idle";
        case StringState::ReleasingFinger: return "releasing";
        case StringState::Moving:          return "moving";
        case StringState::PressingFinger:  return "pressing";
        case StringState::Settling:        return "settling";
        case StringState::ReadyToPluck:    return "ready";
        case StringState::Plucking:        return "plucking";
        case StringState::Sustaining:      return "sustaining";
        case StringState::Damping:         return "damping";
        case StringState::Cancelling:      return "cancelling";
        case StringState::Fault:           return "fault";
        default:                           return "idle";
    }
}

// Whether the finger is pressed for this state (open strings never press).
bool fingerDown(StringState s, bool openString) {
    if (openString) return false;
    switch (s) {
        case StringState::PressingFinger:
        case StringState::Settling:
        case StringState::ReadyToPluck:
        case StringState::Plucking:
        case StringState::Sustaining:
            return true;
        default:
            return false;
    }
}

const char* safetyStateName(SafetyState s) {
    switch (s) {
        case SafetyState::ConfigSafe:    return "configSafe";
        case SafetyState::PowerOnSafe:   return "powerOnSafe";
        case SafetyState::Homing:        return "homing";
        case SafetyState::Armed:         return "armed";
        case SafetyState::Panic:         return "panic";
        case SafetyState::EmergencyStop: return "emergencyStop";
        default:                         return "safe";
    }
}

// The transport an event arrived on. Matches the names /api/status uses for
// `midiSource` and `midiTransports[].name`, so the monitor and the status panel
// can never disagree about what "din" means.
const char* midiSourceName(MidiSource s) {
    switch (s) {
        case MidiSource::Internal:      return "internal";
        case MidiSource::WifiWebSocket: return "wifiWebSocket";
        case MidiSource::WifiRtp:       return "wifiRtp";
        case MidiSource::WifiUdp:       return "wifiUdp";
        case MidiSource::WebUiTest:     return "webUiTest";
        case MidiSource::Ble:           return "ble";
        case MidiSource::Usb:           return "usb";
        case MidiSource::Din:           return "din";
        case MidiSource::Serial:        return "serial";
        default:                        return "other";
    }
}

const char* midiTypeName(uint8_t type) {
    switch (type) {
        case 0x80: return "noteOff";
        case 0x90: return "noteOn";
        case 0xA0: return "polyAftertouch";
        case 0xB0: return "controlChange";
        case 0xC0: return "programChange";
        case 0xD0: return "channelAftertouch";
        case 0xE0: return "pitchBend";
        default: return "other";
    }
}

// Shown at "/" when the firmware carries no embedded UI AND LittleFS has none
// (an empty asset table plus a filesystem that was never uploaded or failed to
// mount). Without it the captive portal redirected EVERY not-found URL —
// including "/" itself — to the portal root, i.e. "/" redirected to "/" forever:
// the browser's ERR_TOO_MANY_REDIRECTS.
const char kMissingUiPage[] PROGMEM =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Stepper-Plucked-Strings-GMB</title>"
    "<style>body{font-family:sans-serif;max-width:38em;margin:2em auto;"
    "padding:0 1em;line-height:1.5}code{background:#eee;padding:0 .3em;"
    "border-radius:3px}</style></head><body>"
    "<h1>Stepper-Plucked-Strings-GMB</h1>"
    "<p><strong>The firmware is running</strong>, but no web interface was found: "
    "this build carries no embedded UI and LittleFS <code>/www</code> is empty or "
    "did not mount.</p>"
    "<p>Either rebuild with the embedded UI:</p><ol>"
    "<li><code>python3 firmware/tools/embed_web_assets.py</code>, then flash the "
    "firmware again.</li></ol>"
    "<p>or upload the filesystem:</p><ol>"
    "<li>In <code>firmware/</code>, run <code>./sync_web_data.sh</code> "
    "(copies <code>web-interface/</code> to <code>firmware/data/www</code>).</li>"
    "<li>PlatformIO <code>pio run -t uploadfs</code>, or the Arduino IDE LittleFS "
    "upload plugin &mdash; see <code>docs/ARDUINO_IDE.md</code>.</li></ol>"
    "<p>The REST API is live: <a href=\"/api/status\">/api/status</a> &middot; "
    "<a href=\"/api/diagnostics\">/api/diagnostics</a></p></body></html>";

}  // namespace

// Single status DTO shared by GET /api/status and the /ws/status broadcast so
// the two never diverge (fixes the frontend/backend status mismatch).
void WebApi::fillStatus(JsonDocument& doc) {
    doc["state"] = ctx_.appState ? ctx_.appState() : "boot";
    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["mode"] = ctx_.net ? ctx_.net->mode() : "unknown";
    wifi["ssid"] = ctx_.profile ? ctx_.profile->network.ssid : "";
    wifi["ip"] = ctx_.net ? ctx_.net->ipAddress() : "";
    wifi["connected"] = ctx_.net ? ctx_.net->connected() : false;
    // Real transport state, not a constant. `midiSource` is the transport that most
    // recently delivered a message — "what is playing this instrument right now" —
    // and `midiTransports` carries the full picture so the UI can show a DIN cable
    // that is wired but silent separately from one that is not wired at all.
    //
    // It is deliberately NOT "the transport with the most messages since boot": that
    // is a lifetime total, and it answered the wrong question in the case that
    // matters. Plug a DIN cable into a machine that has been on Wi-Fi all day and the
    // total still says wifiUdp however hard you play the cable.
    if (ctx_.midiTransports) {
        auto states = ctx_.midiTransports();
        JsonArray arr = doc["midiTransports"].to<JsonArray>();
        for (const auto& t : states) {
            JsonObject o = arr.add<JsonObject>();
            o["name"] = t.name;
            o["label"] = t.label;
            o["bound"] = t.bound;
            o["detail"] = t.detail;
            o["events"] = t.events;          // lifetime total, for diagnostics
            o["lastEventMs"] = t.lastEventMs;  // 0 = never heard from
        }
    }
    // "none" until something is actually received: before the first message there is
    // no active source, and naming one would be a guess.
    doc["midiSource"] = ctx_.lastMidiSource ? ctx_.lastMidiSource() : "none";
    doc["lastMidiEventAtMs"] = ctx_.lastMidiEventMs ? ctx_.lastMidiEventMs() : 0;
    // UDP source posture (audit P1.11) so the Settings UI shows the live state.
    doc["midiSourcePolicy"] = ctx_.midiSourcePolicy ? ctx_.midiSourcePolicy() : "open";
    doc["midiSourceLocked"] = ctx_.midiSourceLocked ? ctx_.midiSourceLocked() : false;
    doc["activeProfile"] = ctx_.profile ? ctx_.profile->instrument.name : "";
    doc["capabilitiesRevision"] =
        ctx_.sysex ? ctx_.sysex->snapshot().revision : 0;
    int total = ctx_.instrument ? static_cast<int>(ctx_.instrument->stringCount()) : 0;
    bool armed = ctx_.safety && ctx_.safety->actuatorsAllowed();
    doc["stringsTotal"] = total;
    // Actually-homed, non-faulted axes (not just "armed => all") so a degraded
    // run reports the true count.
    doc["stringsReady"] = ctx_.readyStrings ? ctx_.readyStrings() : (armed ? total : 0);
    doc["notesPlaying"] = ctx_.instrument ? ctx_.instrument->soundingCount() : 0;
    // Full safety state (powerOnSafe / armed / panic / emergencyStop), not just
    // armed/safe, so the UI can tell a latched panic or E-stop from plain boot.
    doc["safety"] = ctx_.safety ? safetyStateName(ctx_.safety->state())
                                : (armed ? "armed" : "safe");
    doc["authConfigured"] = ctx_.authConfigured ? ctx_.authConfigured() : false;
    // The AP is "open" when it is active and NOT WPA2-secured (real password
    // check, not merely whether the auth callback exists).
    doc["apOpen"] = ctx_.net && ctx_.net->accessPointActive() &&
                    !ctx_.net->accessPointSecured();
    JsonArray faults = doc["faults"].to<JsonArray>();
    if (ctx_.safety)
        for (const auto& f : ctx_.safety->faults()) {
            JsonObject o = faults.add<JsonObject>();
            o["source"] = f.source;
            o["message"] = f.message;
            o["atMs"] = f.atMs;
        }
    JsonArray strings = doc["strings"].to<JsonArray>();
    if (ctx_.instrument && ctx_.steppers) {
        int8_t capo = ctx_.profile ? ctx_.profile->instrument.capo : 0;
        // Both transposes, matching the selector/allocator (audit P1-15).
        int transpose = ctx_.profile ? (ctx_.profile->instrument.transpose +
                                        ctx_.profile->midi.transpose) : 0;
        for (size_t i = 0; i < ctx_.instrument->stringCount(); ++i) {
            const StringController& sc = ctx_.instrument->string(i);
            const StringTarget& tgt = ctx_.instrument->target(i);
            StringState st = sc.state();
            bool open = sc.openString();
            double pos = ctx_.steppers->positionMm(i);
            uint8_t openNote =
                (ctx_.profile && i < ctx_.profile->strings.size())
                    ? ctx_.profile->strings[i].openNote : 0;
            JsonObject s = strings.add<JsonObject>();
            s["index"] = i;
            s["openNote"] = openNote;
            s["state"] = stringStateName(st);
            s["fret"] = tgt.fret;
            s["active"] = tgt.active;
            // MIDI note currently sounding (best-effort from open note + fret).
            if (tgt.active)
                s["note"] = static_cast<int>(openNote) + tgt.fret + capo + transpose;
            else
                s["note"] = nullptr;
            s["positionMm"] = pos;
            s["targetMm"] = tgt.positionMm;
            s["distanceMm"] = tgt.positionMm - pos;
            s["home"] = ctx_.steppers->homeActive(i);
            s["limit"] = ctx_.steppers->limitActive(i);
            s["finger"] = fingerDown(st, open) ? "down" : "up";
            s["plectrum"] = (st == StringState::Plucking) ? "strike" : "rest";
            s["lastFault"] = (st == StringState::Fault) ? "fault" : "none";
        }
    }
}

void WebApi::begin(const WebContext& ctx, uint16_t port) {
    ctx_ = ctx;
    if (server_ == nullptr) server_ = new AsyncWebServer(port);
    registerRoutes();

    statusWs_.onEvent([](AsyncWebSocket*, AsyncWebSocketClient*, AwsEventType,
                         void*, uint8_t*, size_t) {});
    midiWs_.onEvent([](AsyncWebSocket*, AsyncWebSocketClient*, AwsEventType, void*,
                       uint8_t*, size_t) {});
    server_->addHandler(&statusWs_);
    server_->addHandler(&midiWs_);

    // ---- Captive portal ---------------------------------------------------
    // The DNS server in Net answers every name with the AP address, but that is
    // only half of a captive portal: the OS "is there internet?" probes must be
    // answered with a REDIRECT rather than the 204/success they expect, or the
    // phone concludes the network is fine and never pops its sign-in sheet. Without
    // these the hotspot resolved everything to the device and still did not open
    // the config page. Registered before the static handler so they take
    // precedence, and gated on AP mode: in station mode this is a normal host and
    // must not hijack those paths to an unreachable AP address.
    auto redirectToPortal = [this](AsyncWebServerRequest* req) {
        if (ctx_.net && ctx_.net->accessPointActive())
            req->redirect(captivePortalUrl().c_str());
        else
            req->send(404, "text/plain", "Not found");
    };
    for (const char* probe : {"/generate_204", "/gen_204", "/hotspot-detect.html",
                              "/library/test/success.html", "/connecttest.txt",
                              "/ncsi.txt", "/redirect", "/canonical.html"}) {
        server_->on(probe, HTTP_GET, redirectToPortal);
    }

    // Static UI from LittleFS: an OPTIONAL per-file override of the embedded UI
    // below, so the interface can be updated on a device without recompiling.
    // Registered first, so an uploaded /www file always wins over its embedded copy.
    server_->serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html");
    if (!LittleFS.exists("/www/index.html"))
        Serial.println(F("[web] LittleFS /www absent — serving the embedded web UI"));

    // Anything else: the embedded UI first, then in AP mode send it to the portal
    // (this catches the OS probes not named above); otherwise a plain 404.
    server_->onNotFound([this](AsyncWebServerRequest* req) {
        // Web UI embedded in the firmware image (WebAssets.cpp, generated from
        // web-interface/): a plain firmware upload serves the whole interface with
        // no separate LittleFS-upload step. Only consulted on a LittleFS miss.
        std::string path(req->url().c_str());
        if (path == "/") path = "/index.html";
        if (const WebAsset* a = findWebAsset(path.c_str())) {
            AsyncWebServerResponse* res =
                req->beginResponse(200, a->mime, a->data, a->size);
            if (a->gzip) res->addHeader("Content-Encoding", "gzip");
            req->send(res);
            return;
        }
        // Last-resort page when the firmware was built with an empty asset table
        // AND no LittleFS UI is present. Never redirect "/" to itself — that loop
        // is the browser's "too many redirects" error.
        if (req->url() == "/" || req->url() == "/index.html") {
            req->send(200, "text/html", kMissingUiPage);
            return;
        }
        if (ctx_.net && ctx_.net->accessPointActive()) {
            req->redirect(captivePortalUrl().c_str());
        } else {
            req->send(404, "text/plain", "Not found");
        }
    });
    server_->begin();
}

std::string WebApi::captivePortalUrl() const {
    // The ESP32 softAP always answers on 192.168.4.1 (no softAPConfig is set), so
    // use the constant instead of reading Net's ip_ std::string from the async web
    // task — that field is written by the main loop and a cross-task read could
    // tear during an AP/station switch. (If softAPConfig is ever added, update
    // this and the note in docs/NETWORK_HOTSPOT.md.)
    return "http://192.168.4.1/";
}

bool WebApi::authOk(AsyncWebServerRequest* req) {
    if (!ctx_.checkToken) return true;
    std::string tok = std::string(req->header("X-GMB-Token").c_str());
    return ctx_.checkToken(tok);
}

void WebApi::registerRoutes() {
    // ---- GET /api/status ----
    server_->on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
        // Serve the immutable snapshot produced by loop(); never read live state
        // from the async web task.
        std::string s;
        { WebStateLock lk(ctx_); s = cachedStatus_; }
        req->send(200, "application/json", String(s.c_str()));
    });

    // ---- POST /api/auth/check (does this token authorise writes?) ----
    server_->on("/api/auth/check", HTTP_POST, [this](AsyncWebServerRequest* req) {
        JsonDocument doc;
        if (!authOk(req)) {
            doc["ok"] = false;
            doc["error"] = "unauthorized";
            sendJson(req, doc, 401);
            return;
        }
        doc["ok"] = true;
        sendJson(req, doc);
    });

    // ---- POST /api/hotspot (switch to the access point on demand) ----
    server_->on("/api/hotspot", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!authOk(req)) { JsonDocument d; d["ok"] = false; d["error"] = "unauthorized";
                            sendJson(req, d, 401); return; }
        JsonDocument doc;
        if (ctx_.onStartHotspot) ctx_.onStartHotspot();
        doc["ok"] = true;
        doc["note"] = "Switching to access point — rejoin the device's Wi-Fi network.";
        // Serviced on the main loop; the station link (and this response's route home)
        // may drop as the radio switches, which is expected.
        sendJson(req, doc, 202);
    });

    // ---- GET /api/wifi/scan[?start=1] (network picker survey) ----
    server_->on("/api/wifi/scan", HTTP_GET, [this](AsyncWebServerRequest* req) {
        if (!ctx_.wifiScanJson) {
            JsonDocument d; d["ok"] = false; d["error"] = "unsupported";
            sendJson(req, d, 501);
            return;
        }
        if (req->hasParam("start")) {
            if (!authOk(req)) { JsonDocument d; d["ok"] = false; d["error"] = "unauthorized";
                                sendJson(req, d, 401); return; }
            if (ctx_.onWifiScanStart) ctx_.onWifiScanStart();
        }
        req->send(200, "application/json", String(ctx_.wifiScanJson().c_str()));
    });

    // ---- GET /api/diagnostics (runtime telemetry for the bench, P2.19) ----
    // The body is built by loop() via the callback (accumulated counters + the
    // cached PCA health), so the async web task never touches live I2C or state.
    server_->on("/api/diagnostics", HTTP_GET, [this](AsyncWebServerRequest* req) {
        std::string s = ctx_.diagnosticsJson ? ctx_.diagnosticsJson() : "{}";
        req->send(200, "application/json", String(s.c_str()));
    });

    // ---- GET /api/commands?id=N (result of a 202-accepted command) ----
    server_->on("/api/commands", HTTP_GET, [this](AsyncWebServerRequest* req) {
        JsonDocument doc;
        uint32_t id = 0;
        if (req->hasParam("id")) id = req->getParam("id")->value().toInt();
        doc["id"] = id;
        // queued / succeeded / refused / unknown (unknown = never issued or aged
        // out of the small result ring).
        doc["state"] = ctx_.commandState ? ctx_.commandState(id) : "unknown";
        sendJson(req, doc);
    });

    // ---- POST /api/reset (recover from panic / E-stop, then re-home) ----
    server_->on("/api/reset", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!authOk(req)) { JsonDocument d; d["ok"] = false; d["error"] = "unauthorized";
                            sendJson(req, d, 401); return; }
        uint32_t cmdId = ctx_.onReset ? ctx_.onReset() : 0;
        bool queued = cmdId != 0;
        JsonDocument doc;
        doc["ok"] = queued;
        doc["accepted"] = queued;
        doc["commandId"] = cmdId;  // poll GET /api/commands?id=... for the outcome
        doc["note"] = queued ? "reset queued" : "command queue full";
        sendJson(req, doc, queued ? 202 : 503);
    });

    // ---- GET /api/boards (which boards this firmware actually supports) ----
    // The UI builds its board picker from this instead of carrying its own list:
    // a hand-maintained copy is how the picker came to offer one board while the
    // firmware supported four.
    server_->on("/api/boards", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        JsonArray arr = doc["boards"].to<JsonArray>();
        for (const BoardProfile* b : builtinBoardProfiles()) {
            JsonObject o = arr.add<JsonObject>();
            o["identifier"] = b->identifier;
            o["displayName"] = b->displayName;
        }
        sendJson(req, doc);
    });

    // ---- GET /api/board/{id} — one route per built-in board ----
    for (const BoardProfile* b : builtinBoardProfiles()) {
        server_->on(("/api/board/" + b->identifier).c_str(), HTTP_GET,
                    [b](AsyncWebServerRequest* req) {
            JsonDocument doc;
            doc["identifier"] = b->identifier;
            doc["displayName"] = b->displayName;
            JsonArray pins = doc["pins"].to<JsonArray>();
            for (const auto& p : b->pins) {
                JsonObject o = pins.add<JsonObject>();
                o["gpio"] = p.gpio;
                o["preference"] = prefName(p.preference);
                // The FULL capability set, matching board-profiles/*.json field for
                // field. exposed/input/output/interrupt were missing here, and the
                // web UI's pinSupports() bails out on the first falsy one — so
                // against a real device every signal dropdown came up empty while
                // the offline mock (which has the fields) looked fine.
                o["exposed"] = p.exposed;
                o["input"] = p.input;
                o["output"] = p.output;
                o["interrupt"] = p.interrupt;
                o["internalPullUp"] = p.internalPullUp;
                o["internalPullDown"] = p.internalPullDown;
                o["reserved"] = p.reserved;
                o["usb"] = p.usb;
                o["strapping"] = p.strapping;
                o["onboardPeripheral"] = p.onboardPeripheral;
                o["highSpeedOutput"] = p.highSpeedOutput;
                o["adc"] = p.adc;
                o["note"] = p.note;
            }
            sendJson(req, doc);
        });
    }

    // ---- POST /api/pins/auto (auto-assign GPIO for the wizard's draft) ----
    // Reads the wizard's request body so the assignment matches the DRAFT being
    // edited (string count, USB reservation, PCA/OE, LIMIT switches), not the
    // currently-active profile.
    auto* pinsAuto = new AsyncCallbackJsonWebHandler(
        "/api/pins/auto", [this](AsyncWebServerRequest* req, JsonVariant& body) {
            // Assign against the board the request names — the DRAFT's board, not
            // a hard-coded one. This used to always build a PinManager for the
            // ESP32-S3 while the UI's offline mock was board-aware, so the browser
            // demo produced a correct map and the real device handed out S3 pins
            // that may not even exist on the selected board. An unknown board is
            // refused rather than silently substituted.
            std::string boardId = body["board"] | "";
            if (boardId.empty() && ctx_.profile) boardId = ctx_.profile->boardIdentifier;
            const BoardProfile* b = builtinBoardProfile(boardId);
            if (!b) {
                JsonDocument err;
                err["ok"] = false;
                err["error"] = "unknown board \"" + boardId + "\"";
                sendJson(req, err, 422);
                return;
            }
            PinManager pm(*b);
            PinRequest r;
            int fallback = ctx_.profile ? ctx_.profile->instrument.stringCount : 4;
            r.stringCount = body["stringCount"] | fallback;
            r.useI2cServos = body["useI2cServos"] | body["usePca"] | true;
            r.globalEnable = body["globalEnable"] | true;
            r.servoSafetyOe = body["servoSafetyOe"] | body["useOe"] | true;
            r.reserveUsb = body["reserveUsb"] | true;
            r.useLimitSwitches = body["useLimitSwitches"] | body["useLimits"] | false;
            bool ok = pm.autoAssign(r);
            JsonDocument doc;
            doc["ok"] = ok;
            doc["board"] = b->identifier;   // echo it: the caller can verify the target
            JsonArray pins = doc["pins"].to<JsonArray>();
            for (const auto& a : pm.assignments()) {
                JsonObject o = pins.add<JsonObject>();
                o["signal"] = a.signal;
                o["gpio"] = a.gpio;
            }
            sendJson(req, doc, ok ? 200 : 422);
        });
    pinsAuto->setMethod(HTTP_POST);
    server_->addHandler(pinsAuto);

    // ---- POST /api/panic ----
    server_->on("/api/panic", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (ctx_.onPanic) ctx_.onPanic();
        JsonDocument doc;
        doc["ok"] = true;
        sendJson(req, doc);
    });

    // ---- GET /api/capabilities ----
    server_->on("/api/capabilities", HTTP_GET, [this](AsyncWebServerRequest* req) {
        JsonDocument doc;
        WebStateLock lk(ctx_);  // snapshot may be rebuilt by loop() on a fault
        if (ctx_.sysex) {
            const CapabilitySnapshot& s = ctx_.sysex->snapshot();
            doc["revision"] = s.revision;
            doc["valid"] = s.valid;  // false => no playable string (degraded to nil)
            doc["noteMin"] = s.capabilities.noteMin;
            doc["noteMax"] = s.capabilities.noteMax;
            doc["polyphony"] = s.capabilities.polyphony;
            doc["ccString"] = s.stringConfig.ccString;
            doc["ccFret"] = s.stringConfig.ccFret;
            JsonArray cc = doc["cc"].to<JsonArray>();
            for (uint8_t c : s.capabilities.supportedCc) cc.add(c);
            JsonArray tuning = doc["tuning"].to<JsonArray>();
            for (uint8_t t : s.stringConfig.tuning) tuning.add(t);
        }
        sendJson(req, doc);
    });

    // ---- GET /gmb/descriptor.json (GMB v2 level-1 descriptor over HTTP) ----
    // Advertised by the SysEx handshake `flags` bit 0 so a GMB controller can fetch
    // the whole descriptor in one request instead of the segmented block 0x10
    // transfer. Served from the same immutable snapshot as the SysEx path.
    server_->on("/gmb/descriptor.json", HTTP_GET, [this](AsyncWebServerRequest* req) {
        std::string json;
        { WebStateLock lk(ctx_); if (ctx_.sysex) json = ctx_.sysex->descriptorJson(); }
        req->send(200, "application/json", String(json.c_str()));
    });

    // ---- GET /api/profile ----
    server_->on("/api/profile", HTTP_GET, [this](AsyncWebServerRequest* req) {
        JsonDocument doc;
        { WebStateLock lk(ctx_);  // g_profile may be swapped by loop() on activate
          if (ctx_.profile) ProfileStorage::toJson(*ctx_.profile, doc); }
        sendJson(req, doc);
    });

    // ---- GET /api/profiles (slot list) ----
    server_->on("/api/profiles", HTTP_GET, [this](AsyncWebServerRequest* req) {
        JsonDocument doc;
        JsonArray arr = doc["profiles"].to<JsonArray>();
        if (ctx_.storage) {
            WebStorageLock sl(ctx_);  // don't cross a concurrent save/delete (P1-2)
            auto names = ctx_.storage->list();
            for (size_t i = 0; i < names.size(); ++i) {
                JsonObject o = arr.add<JsonObject>();
                o["slot"] = i;
                o["name"] = names[i];
                o["used"] = !names[i].empty();
            }
            doc["startupSlot"] = ctx_.storage->startupSlot();
        }
        sendJson(req, doc);
    });

    // Body-parsing endpoints.
    auto validate = [this](const JsonVariant& body, JsonDocument& out) {
        Profile p;
        if (!ProfileStorage::fromJson(body, p)) {
            out["ok"] = false;
            out["error"] = "invalid profile JSON";
            return;
        }
        auto issues = ProfileValidator::validate(p);
        JsonArray errs = out["issues"].to<JsonArray>();
        bool ok = true;
        for (const auto& is : issues) {
            JsonObject o = errs.add<JsonObject>();
            o["field"] = is.field;
            o["message"] = is.message;
            o["severity"] =
                is.severity == ValidationIssue::Severity::Error ? "error" : "warning";
            if (is.severity == ValidationIssue::Severity::Error) ok = false;
        }
        out["ok"] = ok;
    };

    // ---- PUT /api/profile (validate + activate) ----
    auto* putProfile = new AsyncCallbackJsonWebHandler(
        "/api/profile", [this](AsyncWebServerRequest* req, JsonVariant& body) {
            if (!authOk(req)) { JsonDocument d; d["ok"] = false; d["error"] = "unauthorized"; sendJson(req, d, 401); return; }
            Profile p;
            JsonDocument doc;
            if (!ProfileStorage::fromJson(body, p)) {
                doc["ok"] = false;
                doc["error"] = "invalid profile JSON";
                sendJson(req, doc, 400);
                return;
            }
            auto issues = ProfileValidator::validate(p);
            bool ok = true;
            JsonArray errs = doc["issues"].to<JsonArray>();
            for (const auto& is : issues) {
                if (is.severity == ValidationIssue::Severity::Error) ok = false;
                JsonObject o = errs.add<JsonObject>();
                o["field"] = is.field;
                o["message"] = is.message;
                o["severity"] =
                    is.severity == ValidationIssue::Severity::Error ? "error" : "warning";
            }
            if (!ok) {
                doc["ok"] = false;
                sendJson(req, doc, 422);  // real error, not masked as success
                return;
            }
            // PERSIST AND ACTIVATE, or neither — see publishProfile().
            // This is the draft the user edited FOR THIS MACHINE (pins, board and
            // network included), so it is taken whole: keepDeviceConfig = false.
            // The activation itself runs in loop() (motor stop, reconfigure,
            // re-home), so the answer is ACCEPTED, not "done".
            PublishResult pr = publishProfile(p, /*keepDeviceConfig=*/false);
            doc["ok"] = pr.accepted;
            doc["accepted"] = pr.accepted;
            doc["persisted"] = pr.persisted;
            doc["commandId"] = pr.commandId;
            if (pr.error) doc["error"] = pr.error;
            doc["note"] = pr.accepted ? "activation queued and stored" : "not published";
            sendJson(req, doc, pr.httpStatus);
        });
    putProfile->setMethod(HTTP_PUT);
    server_->addHandler(putProfile);

    // ---- POST /api/profile/normalize (canonical profile + issues) ----
    //
    // The firmware owns the schema: the version migrations, the per-field defaults
    // and the cross-field rules all live here. The web interface was re-deriving a
    // subset of that in JS (ensureProfileDefaults), and only at the TOP level — so
    // `{"stringFretSelection": {"enabled": true}}` parsed into a complete profile
    // here while the browser kept an object with no `.string`, and the MIDI panel
    // then read `sfs.string.ccNumber` off undefined.
    //
    // Rather than grow a second migration in JS, this returns the profile as the
    // firmware understands it — migrated, defaulted, complete — and the UI adopts
    // exactly that. Validation issues come back in the same round trip, because
    // "normalise it" and "is it acceptable?" are the same question at import time.
    //
    // 200 even when the profile has blocking errors: the caller asked what this
    // file MEANS, and the answer plus the reasons is more useful than a refusal.
    auto* normalizeProfile = new AsyncCallbackJsonWebHandler(
        "/api/profile/normalize", [this](AsyncWebServerRequest* req, JsonVariant& body) {
            JsonDocument doc;
            JsonDocument raw;
            raw.set(body);
            ProfileStorage::migrate(raw);   // v1 -> v2 -> ... before parsing
            Profile p;
            if (!ProfileStorage::fromJson(raw.as<JsonVariantConst>(), p)) {
                doc["ok"] = false;
                doc["error"] = "invalid profile JSON";
                sendJson(req, doc, 400);
                return;
            }
            bool ok = true;
            JsonArray errs = doc["issues"].to<JsonArray>();
            for (const auto& is : ProfileValidator::validate(p)) {
                if (is.severity == ValidationIssue::Severity::Error) ok = false;
                JsonObject o = errs.add<JsonObject>();
                o["field"] = is.field;
                o["message"] = is.message;
                o["severity"] =
                    is.severity == ValidationIssue::Severity::Error ? "error" : "warning";
            }
            doc["ok"] = ok;
            // toJson builds a whole document; nest it (ArduinoJson deep-copies on
            // cross-document assignment, so this is a full independent copy).
            JsonDocument canonical;
            ProfileStorage::toJson(p, canonical);
            doc["profile"] = canonical;
            sendJson(req, doc, 200);
        });
    normalizeProfile->setMethod(HTTP_POST);
    server_->addHandler(normalizeProfile);

    // ---- POST /api/pins/validate (full-profile validation) ----
    auto* validatePins = new AsyncCallbackJsonWebHandler(
        "/api/pins/validate",
        [this, validate](AsyncWebServerRequest* req, JsonVariant& body) {
            JsonDocument doc;
            validate(body, doc);
            sendJson(req, doc, doc["ok"] == true ? 200 : 422);
        });
    validatePins->setMethod(HTTP_POST);
    server_->addHandler(validatePins);

    // ---- POST /api/profiles (save to a slot) ----
    auto* saveProfile = new AsyncCallbackJsonWebHandler(
        "/api/profiles", [this](AsyncWebServerRequest* req, JsonVariant& body) {
            if (!authOk(req)) { JsonDocument d; d["ok"] = false; d["error"] = "unauthorized"; sendJson(req, d, 401); return; }
            JsonDocument doc;
            int slot = body["slot"] | -1;
            Profile p;
            // Copy the live profile (when no body profile) under the STATE lock —
            // brief, in-memory. The actual flash write below is under the STORAGE
            // lock so loop() never waits on it.
            bool parsed;
            { WebStateLock lk(ctx_);
              parsed = body["profile"].is<JsonObject>()
                           ? ProfileStorage::fromJson(body["profile"], p)
                           : (ctx_.profile ? (p = *ctx_.profile, true) : false); }
            if (slot < 0 || !parsed || !ctx_.storage) {
                doc["ok"] = false;
                doc["error"] = "slot and profile required";
                sendJson(req, doc, 400);
                return;
            }
            bool saved;
            { WebStorageLock sl(ctx_);  // serialise LittleFS; loop() never waits here
              saved = ctx_.storage->save(slot, p);
              if (saved && (body["startup"] | false)) ctx_.storage->setStartupSlot(slot); }
            doc["ok"] = saved;
            sendJson(req, doc, saved ? 200 : 500);
        });
    saveProfile->setMethod(HTTP_POST);
    server_->addHandler(saveProfile);

    // ---- POST /api/profiles/load (activate a stored slot) ----
    auto* loadProfile = new AsyncCallbackJsonWebHandler(
        "/api/profiles/load", [this](AsyncWebServerRequest* req, JsonVariant& body) {
            if (!authOk(req)) { JsonDocument d; d["ok"] = false; d["error"] = "unauthorized"; sendJson(req, d, 401); return; }
            JsonDocument doc;
            int slot = body["slot"] | -1;
            Profile p;
            bool loaded;
            { WebStorageLock sl(ctx_);  // serialise LittleFS (loop never waits here)
              loaded = slot >= 0 && ctx_.storage && ctx_.storage->load(slot, p); }
            if (!loaded) {
                doc["ok"] = false;
                doc["error"] = "slot not found";
                sendJson(req, doc, 404);
                return;
            }
            // Loading a stored slot swaps the INSTRUMENT; this machine keeps its
            // own device config (board, pins, network, E-stop wiring, fitted
            // hardware). Taking the slot's device half would, among other things,
            // report a network the radio is not on and adopt another machine's
            // E-stop polarity.
            //
            // Persist the result as the running instrument so the choice survives a
            // reboot — but store THIS machine's device half merged with the slot's
            // instrument, exactly as the runtime is about to do. Storing the slot's
            // own device half would reintroduce, on disk, the very swap this route
            // exists to prevent.
            //
            // Same transaction as PUT /api/profile, and the merge is done ONCE:
            // publishProfile stores whatever the reservation resolved, which
            // is the profile that will run. This route used to redo the merge here
            // to build its own `toStore`, so "what runs" and "what is stored" were
            // two computations that had to agree.
            PublishResult pr = publishProfile(p, /*keepDeviceConfig=*/true);
            doc["ok"] = pr.accepted;
            doc["accepted"] = pr.accepted;
            doc["persisted"] = pr.persisted;
            doc["commandId"] = pr.commandId;
            if (pr.error) doc["error"] = pr.error;
            doc["note"] = pr.accepted
                              ? "instrument activation queued and stored (device config kept)"
                              : "not published";
            sendJson(req, doc, pr.httpStatus);
        });
    loadProfile->setMethod(HTTP_POST);
    server_->addHandler(loadProfile);

    // ---- POST /api/profiles/read (read a slot WITHOUT activating it) ----
    // Used by copy / rename / set-startup so an admin action never moves a motor.
    auto* readProfile = new AsyncCallbackJsonWebHandler(
        "/api/profiles/read", [this](AsyncWebServerRequest* req, JsonVariant& body) {
            int slot = body["slot"] | -1;
            Profile p;
            bool loaded;
            { WebStorageLock sl(ctx_);  // serialise LittleFS (loop never waits here)
              loaded = slot >= 0 && ctx_.storage && ctx_.storage->load(slot, p); }
            if (!loaded) {
                JsonDocument err;
                err["ok"] = false;
                err["error"] = "slot not found";
                sendJson(req, err, 404);
                return;
            }
            JsonDocument doc;
            ProfileStorage::toJson(p, doc);  // returned as-is, not activated
            sendJson(req, doc);
        });
    readProfile->setMethod(HTTP_POST);
    server_->addHandler(readProfile);

    // ---- POST /api/profiles/delete ----
    auto* deleteProfile = new AsyncCallbackJsonWebHandler(
        "/api/profiles/delete", [this](AsyncWebServerRequest* req, JsonVariant& body) {
            if (!authOk(req)) { JsonDocument d; d["ok"] = false; d["error"] = "unauthorized"; sendJson(req, d, 401); return; }
            JsonDocument doc;
            int slot = body["slot"] | -1;
            WebStorageLock sl(ctx_);  // serialise LittleFS (loop never waits here)
            bool ok = slot >= 0 && ctx_.storage && ctx_.storage->remove(slot);
            doc["ok"] = ok;
            sendJson(req, doc, ok ? 200 : 400);
        });
    deleteProfile->setMethod(HTTP_POST);
    server_->addHandler(deleteProfile);

    // ---- POST /api/test/note (Note On + scheduled Note Off, Ready only) ----
    auto* testNote = new AsyncCallbackJsonWebHandler(
        "/api/test/note", [this](AsyncWebServerRequest* req, JsonVariant& body) {
            if (!authOk(req)) { JsonDocument d; d["ok"] = false; d["error"] = "unauthorized"; sendJson(req, d, 401); return; }
            JsonDocument doc;
            // ccString / ccFret are the SELECTION CC values a controller would
            // send (0..127). Absent means "no selection CC" — the note is then
            // allocated automatically, exactly as before.
            int ccString = body["ccString"].isNull() ? -1 : (body["ccString"] | -1);
            int ccFret = body["ccFret"].isNull() ? -1 : (body["ccFret"] | -1);
            if (ccString > 127 || ccFret > 127) {
                doc["ok"] = false;
                doc["error"] = "ccString/ccFret must be 0-127";
                sendJson(req, doc, 422);
                return;
            }
            uint32_t cmdId = ctx_.onTestNote
                ? ctx_.onTestNote(body["channel"] | 0, body["note"] | 60,
                                  body["velocity"] | 100, body["durationMs"] | 500,
                                  ccString, ccFret)
                : 0;
            bool queued = cmdId != 0;
            doc["ok"] = queued;
            doc["accepted"] = queued;
            doc["commandId"] = cmdId;
            doc["note"] = queued ? "test note queued" : "command queue full";
            sendJson(req, doc, queued ? 202 : 503);
        });
    testNote->setMethod(HTTP_POST);
    server_->addHandler(testNote);

    // ---- POST /api/test/servo (pulse a servo to rest/active) ----
    auto* testServo = new AsyncCallbackJsonWebHandler(
        "/api/test/servo", [this](AsyncWebServerRequest* req, JsonVariant& body) {
            if (!authOk(req)) { JsonDocument d; d["ok"] = false; d["error"] = "unauthorized"; sendJson(req, d, 401); return; }
            JsonDocument doc;
            if (!ctx_.safety || !ctx_.safety->actuatorsAllowed()) {
                doc["ok"] = false;
                doc["error"] = "actuators not armed";
                sendJson(req, doc, 409);
                return;
            }
            // Never drive a servo from the async task: enqueue for loop(), which
            // also rejects an invalid / disabled index.
            int idx = body["index"] | -1;
            bool active = body["active"] | true;
            uint32_t cmdId = ctx_.onTestServo ? ctx_.onTestServo(idx, active) : 0;
            bool queued = cmdId != 0;
            doc["ok"] = queued;
            doc["accepted"] = queued;
            doc["commandId"] = cmdId;
            doc["note"] = queued ? "servo test queued" : "command queue full";
            sendJson(req, doc, queued ? 202 : 503);
        });
    testServo->setMethod(HTTP_POST);
    server_->addHandler(testServo);

    // ---- POST /api/test/jog (nudge one axis by a delta, Ready only) ----
    auto* testJog = new AsyncCallbackJsonWebHandler(
        "/api/test/jog", [this](AsyncWebServerRequest* req, JsonVariant& body) {
            if (!authOk(req)) { JsonDocument d; d["ok"] = false; d["error"] = "unauthorized"; sendJson(req, d, 401); return; }
            JsonDocument doc;
            if (!ctx_.safety || !ctx_.safety->actuatorsAllowed()) {
                doc["ok"] = false;
                doc["error"] = "actuators not armed";
                sendJson(req, doc, 409);
                return;
            }
            // Enqueue for loop(): it owns the steppers and also rejects an invalid
            // axis, a faulted axis, or an axis busy with a live note.
            int axis = body["axis"] | -1;
            double delta = body["deltaMm"] | 0.0;
            uint32_t cmdId = ctx_.onJog ? ctx_.onJog(axis, delta) : 0;
            bool queued = cmdId != 0;
            doc["ok"] = queued;
            doc["accepted"] = queued;
            doc["commandId"] = cmdId;
            doc["note"] = queued ? "jog queued" : "command queue full";
            sendJson(req, doc, queued ? 202 : 503);
        });
    testJog->setMethod(HTTP_POST);
    server_->addHandler(testJog);

    // ---- POST /api/test/moveto (send one axis to an absolute mm, Ready only) ----
    auto* testMoveTo = new AsyncCallbackJsonWebHandler(
        "/api/test/moveto", [this](AsyncWebServerRequest* req, JsonVariant& body) {
            if (!authOk(req)) { JsonDocument d; d["ok"] = false; d["error"] = "unauthorized"; sendJson(req, d, 401); return; }
            JsonDocument doc;
            if (!ctx_.safety || !ctx_.safety->actuatorsAllowed()) {
                doc["ok"] = false;
                doc["error"] = "actuators not armed";
                sendJson(req, doc, 409);
                return;
            }
            // Same enqueue-for-loop() contract as the jog: loop() owns the
            // steppers and applies the identical per-axis gate. The target is
            // measured from the homing zero and clamped to the axis travel.
            int axis = body["axis"] | -1;
            double positionMm = body["positionMm"] | 0.0;
            uint32_t cmdId = ctx_.onMoveTo ? ctx_.onMoveTo(axis, positionMm) : 0;
            bool queued = cmdId != 0;
            doc["ok"] = queued;
            doc["accepted"] = queued;
            doc["commandId"] = cmdId;
            doc["note"] = queued ? "move queued" : "command queue full";
            sendJson(req, doc, queued ? 202 : 503);
        });
    testMoveTo->setMethod(HTTP_POST);
    server_->addHandler(testMoveTo);

    // ---- POST /api/test/endstop (read a HOME/LIMIT sensor) ----
    auto* testEndstop = new AsyncCallbackJsonWebHandler(
        "/api/test/endstop", [this](AsyncWebServerRequest* req, JsonVariant& body) {
            JsonDocument doc;
            if (!ctx_.steppers) {
                doc["ok"] = false;
                sendJson(req, doc, 409);
                return;
            }
            size_t axis = body["axis"] | 0;
            WebStateLock lk(ctx_);  // steppers vector may be rebuilt by loop()
            doc["ok"] = true;
            doc["home"] = ctx_.steppers->homeActive(axis);
            doc["limit"] = ctx_.steppers->limitActive(axis);
            // The RAW level and the declared sensor type alongside the normalised
            // reading, because "not triggered" is the same answer as "polarity is
            // backwards" until you can see both. That distinction matters most on an
            // optical gate, whose output usually idles the opposite way round from
            // the switch it replaced.
            doc["homeRaw"] = ctx_.steppers->homeRawHigh(axis) ? "high" : "low";
            if (ctx_.profile && axis < ctx_.profile->homing.size()) {
                const HomingConfig& hc = ctx_.profile->homing[axis];
                doc["homeSensor"] = endstopTypeName(hc.homeSensor);
                doc["limitSensor"] = endstopTypeName(hc.limitSensor);
                doc["homeDebounceMs"] = endstopDebounceMs(hc.homeSensor);
            }
            sendJson(req, doc);
        });
    testEndstop->setMethod(HTTP_POST);
    server_->addHandler(testEndstop);

    // ---- POST /api/wifi (store credentials in NVS, never exported) ----
    auto* setWifi = new AsyncCallbackJsonWebHandler(
        "/api/wifi", [this](AsyncWebServerRequest* req, JsonVariant& body) {
            if (!authOk(req)) { JsonDocument d; d["ok"] = false; d["error"] = "unauthorized"; sendJson(req, d, 401); return; }
            JsonDocument doc;
            if (!ctx_.onSetWifi) {
                doc["ok"] = false;
                sendJson(req, doc, 409);
                return;
            }
            WebContext::WifiRequest rq;
            // Only overwrite a password that was actually provided (an absent or
            // empty field leaves the stored secret unchanged). Erasing one is a
            // separate, explicit flag for exactly that reason.
            rq.stationPassword = body["stationPassword"] | "";
            rq.apPassword = body["apPassword"] | "";
            rq.hasStationPassword = !rq.stationPassword.empty();
            rq.hasApPassword = !rq.apPassword.empty();
            rq.clearStationPassword = body["clearStationPassword"] | false;
            rq.clearApPassword = body["clearApPassword"] | false;
            rq.apply = body["apply"] | false;

            // WPA2 bounds. Refusing here matters: a 4-character "password" would
            // silently start an OPEN access point, which is not what was asked for.
            if (rq.hasApPassword && (rq.apPassword.size() < 8 || rq.apPassword.size() > 63)) {
                doc["ok"] = false;
                doc["error"] = "apPassword must be 8-63 characters (WPA2)";
                sendJson(req, doc, 422);
                return;
            }
            // Setting and clearing the same secret in one request is a caller bug,
            // not something to resolve by guessing which one wins.
            if ((rq.hasApPassword && rq.clearApPassword) ||
                (rq.hasStationPassword && rq.clearStationPassword)) {
                doc["ok"] = false;
                doc["error"] = "cannot set and clear the same password";
                sendJson(req, doc, 422);
                return;
            }

            if (!body["mode"].isNull()) {
                std::string mode = body["mode"] | "";
                if (mode != "accessPoint" && mode != "station") {
                    doc["ok"] = false;
                    doc["error"] = "mode must be \"accessPoint\" or \"station\"";
                    sendJson(req, doc, 422);
                    return;
                }
                rq.hasNetwork = true;
                rq.network.mode = mode == "station" ? NetworkMode::Station
                                                    : NetworkMode::AccessPoint;
                rq.network.ssid = body["ssid"] | "";
                rq.network.apSsid = body["apSsid"] | "";
                rq.network.hostname = body["hostname"] | "";
                if (rq.network.apSsid.empty()) {
                    doc["ok"] = false;
                    doc["error"] = "apSsid must not be empty";
                    sendJson(req, doc, 422);
                    return;
                }
                if (rq.network.hostname.empty()) rq.network.hostname = "gmb-instrument";
                // Joining a network needs a name to join.
                if (rq.network.mode == NetworkMode::Station && rq.network.ssid.empty()) {
                    doc["ok"] = false;
                    doc["error"] = "station mode needs an ssid";
                    sendJson(req, doc, 422);
                    return;
                }
            }

            // Report what happened, field by field. This used to be a flat
            // `ok:true` + `applied:<what was asked>` with the outcome buried in a
            // prose note, so a flash failure came back as a 200 that also claimed
            // the radio was reconfiguring — and with apply:true it had not even
            // been asked, because the callback returned before setting the flag.
            WifiResult wr = ctx_.onSetWifi(rq);
            doc["ok"] = wr.ok;
            doc["persisted"] = wr.persisted;
            doc["applied"] = wr.applied;
            doc["note"] = wr.note;
            if (wr.error) doc["error"] = wr.error;
            sendJson(req, doc, wr.httpStatus);
        });
    setWifi->setMethod(HTTP_POST);
    server_->addHandler(setWifi);

    // ---- POST /api/midi/source (UDP MIDI source posture) ----
    // Body: { policy: "open"|"lockToFirst"|"disabled" (optional), unlock: bool }.
    // The Settings > Security panel has always offered these controls; the route
    // did not exist, so the browser mock accepted them and the real device 404ed.
    // The policy is stored in NVS (device state, survives reboots) and applied by
    // the main loop, which owns the MIDI transport.
    auto* midiSource = new AsyncCallbackJsonWebHandler(
        "/api/midi/source", [this](AsyncWebServerRequest* req, JsonVariant& body) {
            if (!authOk(req)) { JsonDocument d; d["ok"] = false; d["error"] = "unauthorized"; sendJson(req, d, 401); return; }
            JsonDocument doc;
            if (!ctx_.onSetMidiSource) {
                doc["ok"] = false;
                doc["error"] = "not supported by this build";
                sendJson(req, doc, 409);
                return;
            }
            int policy = -1;   // -1 = leave the stored policy alone
            if (!body["policy"].isNull()) {
                std::string p = body["policy"] | "";
                if (p == "open") policy = 0;
                else if (p == "lockToFirst") policy = 1;
                else if (p == "disabled") policy = 2;
                else {
                    doc["ok"] = false;
                    doc["error"] = "policy must be open, lockToFirst or disabled";
                    sendJson(req, doc, 422);
                    return;
                }
            }
            const bool unlock = body["unlock"] | false;
            if (!ctx_.onSetMidiSource(policy, unlock)) {
                doc["ok"] = false;
                doc["error"] = "storing the MIDI source policy failed (NVS write error)";
                sendJson(req, doc, 500);
                return;
            }
            doc["ok"] = true;
            sendJson(req, doc);
        });
    midiSource->setMethod(HTTP_POST);
    server_->addHandler(midiSource);

    // ---- POST /api/auth (set the admin token; first-run bootstrap allowed) ----
    auto* setAuth = new AsyncCallbackJsonWebHandler(
        "/api/auth", [this](AsyncWebServerRequest* req, JsonVariant& body) {
            JsonDocument doc;
            if (!authOk(req)) { doc["ok"] = false; doc["error"] = "unauthorized";
                                sendJson(req, doc, 401); return; }
            if (!ctx_.onSetAdminToken) { doc["ok"] = false; sendJson(req, doc, 409); return; }
            std::string t = body["token"] | "";
            // Reject an empty/too-short token: storing "" would silently leave the
            // write routes unauthenticated (first-run bootstrap state).
            if (t.size() < 8) {
                doc["ok"] = false;
                doc["error"] = "token must be at least 8 characters";
                sendJson(req, doc, 422);
                return;
            }
            ctx_.onSetAdminToken(t);
            doc["ok"] = true;
            doc["note"] = "admin token stored";
            sendJson(req, doc);
        });
    setAuth->setMethod(HTTP_POST);
    server_->addHandler(setAuth);

    // ---- POST /api/storage/format (deliberate LittleFS reformat) ----
    server_->on("/api/storage/format", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!authOk(req)) { JsonDocument d; d["ok"] = false; d["error"] = "unauthorized";
                            sendJson(req, d, 401); return; }
        bool ok;
        { WebStorageLock sl(ctx_);  // serialise with other LittleFS operations
          ok = ctx_.onFormatStorage && ctx_.onFormatStorage(); }
        JsonDocument doc;
        doc["ok"] = ok;
        if (!ok) doc["error"] = "format failed or unavailable";
        sendJson(req, doc, ok ? 200 : 500);
    });

    // ---- POST /api/sysex/request (run a SysEx buffer through the service) ----
    auto* sysexReq = new AsyncCallbackJsonWebHandler(
        "/api/sysex/request", [this](AsyncWebServerRequest* req, JsonVariant& body) {
            JsonDocument doc;
            if (!ctx_.sysex) {
                doc["ok"] = false;
                sendJson(req, doc, 409);
                return;
            }
            std::vector<uint8_t> in;
            for (JsonVariant v : body["bytes"].as<JsonArray>())
                in.push_back(static_cast<uint8_t>(v.as<int>() & 0xFF));
            // Serialise with loop()'s UDP SysEx handling: the service shares a rate
            // limiter and snapshot that are not internally synchronised.
            std::vector<uint8_t> out;
            { WebStateLock lk(ctx_);
              out = ctx_.sysex->handleMessage(in.data(), in.size(), millis()); }
            doc["ok"] = true;
            JsonArray resp = doc["response"].to<JsonArray>();
            for (uint8_t b : out) resp.add(b);
            sendJson(req, doc);
        });
    sysexReq->setMethod(HTTP_POST);
    server_->addHandler(sysexReq);
}

// Built by loop() (the state owner); serialised once and cached under the state
// lock so the async web task only ever reads the immutable copy.
void WebApi::refreshStatus() {
    JsonDocument doc;
    fillStatus(doc);
    String out;
    serializeJson(doc, out);
    if (ctx_.lockState) ctx_.lockState();
    cachedStatus_ = std::string(out.c_str());
    if (ctx_.unlockState) ctx_.unlockState();
}

void WebApi::broadcastStatus() {
    if (statusWs_.count() == 0) return;
    std::string s;
    if (ctx_.lockState) ctx_.lockState();
    s = cachedStatus_;
    if (ctx_.unlockState) ctx_.unlockState();
    statusWs_.textAll(String(s.c_str()));
}

void WebApi::broadcastMidi(const MidiEvent& e) {
    if (midiWs_.count() == 0) return;
    JsonDocument doc;
    doc["timestampUs"] = e.timestampUs;
    // The event carries its own transport and its own sender. This used to be the
    // constant "wifiUdp" for every event, so the one tool for diagnosing a
    // multi-controller rig labelled DIN, USB and web-test traffic as Wi-Fi — the
    // exact confusion the monitor exists to resolve.
    doc["source"] = midiSourceName(static_cast<MidiSource>(e.source));
    // The ORIGIN as well as the transport, because two laptops on the same Wi-Fi
    // are the same transport and different senders, and telling them apart is why
    // origins exist at all.
    doc["origin"] = e.origin;
    doc["channel"] = e.channel;          // zero-based on the wire, as MIDI defines it
    doc["type"] = midiTypeName(e.type);
    doc["data1"] = e.data1;
    doc["data2"] = e.data2;
    String out;
    serializeJson(doc, out);
    midiWs_.textAll(out);
}

#else  // non-Arduino: no-op so the file is analysable off-target.

void WebApi::begin(const WebContext& ctx, uint16_t) { ctx_ = ctx; }
void WebApi::refreshStatus() {}
void WebApi::broadcastStatus() {}

#endif

// ---- the publish transaction ------------------------------------------------
//
// Outside the ARDUINO guard on purpose: it touches nothing but the context
// callbacks and ProfileStorage, both of which exist in either build, and it is the
// piece worth exercising on a host.
//
// The ordering is the whole content of this function:
//
//     reserve   validate + merge + claim a queue slot, NOT yet runnable
//     prepare   write and verify /active.json.tmp — nothing visible has changed
//     commit    one rename — the point of no return
//     publish   only now can loop() see the command
//
// Every early return releases the reservation, so the two observable outcomes are
// "persisted and accepted" or "nothing happened". The previous order — reserve,
// prepare, ENQUEUE, commit — could reach a third: the activation was already
// running when the rename failed, leaving the machine on the new profile and the
// flash on the old one. That was reported as a warning, which is honest but is
// still a machine that comes back as something other than what it is running.
//
// INVARIANT to preserve when editing: exactly one of onPublishActivation /
// onCancelActivation is called on every path after a successful reserve. The
// reservation is what excludes a second concurrent publish, so leaking one wedges
// every later publish until reboot.
WebApi::PublishResult WebApi::publishProfile(const Profile& p,
                                              bool keepDeviceConfig) {
    PublishResult r;
    if (!ctx_.storage) {
        r.error = "no storage";
        r.httpStatus = 507;
        return r;
    }
    if (!ctx_.onReserveActivation || !ctx_.onPublishActivation || !ctx_.onCancelActivation) {
        r.error = "activation unavailable";
        r.httpStatus = 503;
        return r;
    }

    // The merged profile comes back from the reservation: it is what will run, so
    // it is what gets stored. Building it separately here — as the load route used
    // to — is two merges that have to agree.
    Profile merged;
    ReserveResult res = ctx_.onReserveActivation(p, keepDeviceConfig, merged);
    if (!res.ok()) {
        // The three reasons are NOT interchangeable, and answering 503 for all of
        // them told a user to retry something that will fail identically forever.
        // An instrument that does not fit this machine's pins is a 422: the fix is
        // to change the profile, not to wait.
        switch (res.status) {
            case ReserveStatus::InvalidProfile:
                r.error = "this profile cannot run on this device — check the pin map "
                          "and the axis configuration";
                r.httpStatus = 422;
                break;
            case ReserveStatus::QueueFull:
                r.error = "the command queue is full — retry in a moment";
                r.httpStatus = 503;
                break;
            default:
                r.error = "another configuration write is in progress — retry in a moment";
                r.httpStatus = 503;
                break;
        }
        return r;
    }
    uint32_t token = res.token;

    bool prepared;
    { WebStorageLock sl(ctx_); prepared = ctx_.storage->prepareActive(merged); }
    if (!prepared) {
        ctx_.onCancelActivation(token);
        r.error = "could not write the configuration to storage — not activating it";
        r.httpStatus = 507;  // Insufficient Storage
        return r;
    }

    bool committed;
    { WebStorageLock sl(ctx_);
      committed = ctx_.storage->commitActive();
      if (!committed) ctx_.storage->discardActive(); }
    if (!committed) {
        ctx_.onCancelActivation(token);
        r.error = "could not commit the configuration to storage — not activating it";
        r.httpStatus = 507;
        return r;
    }

    // Past the rename. The stored snapshot IS this profile now, and the reservation
    // guaranteed the queue has room, so the publish cannot fail for capacity — the
    // only way it returns false is a token this coordinator does not recognise,
    // i.e. a caller bug rather than a runtime condition.
    //
    // It is reported as ACCEPTED anyway, deliberately. The alternative is a third
    // observable state (persisted, not accepted) that the whole design exists to
    // eliminate, and it would be a LIE in the direction that matters: the profile
    // IS on flash, so the machine will come back as it. Saying "not accepted" would
    // tell the operator their change was discarded when the next boot will run it.
    // The mismatch is surfaced as a warning instead, and runtimecheck forces this
    // branch to prove it stays a warning rather than becoming a refusal.
    if (!ctx_.onPublishActivation(token)) {
        r.persisted = true;
        r.accepted = true;
        r.commandId = 0;   // there is no command to follow
        r.error = "stored, but the activation could not be queued — it takes effect "
                  "at the next reboot";
        r.httpStatus = 202;
        return r;
    }
    r.accepted = true;
    r.persisted = true;
    r.commandId = token;
    r.httpStatus = 202;
    return r;
}

}  // namespace gmb
