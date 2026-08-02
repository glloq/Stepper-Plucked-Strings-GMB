#include "WebApi.h"

#include "ProfileStorage.h"
#include "../../core/configuration/ProfileValidator.h"

#if defined(ARDUINO)
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <LittleFS.h>
#endif

namespace gmb {

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
    doc["midiSource"] = "wifiUdp";
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
    doc["safety"] = armed ? "armed" : "safe";
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
        int8_t transpose = ctx_.profile ? ctx_.profile->midi.transpose : 0;
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

    // Static UI from LittleFS (uploaded from web-interface/ via data/www).
    server_->serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html");
    server_->begin();
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

    // ---- POST /api/reset (recover from panic / E-stop, then re-home) ----
    server_->on("/api/reset", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!authOk(req)) { JsonDocument d; d["ok"] = false; d["error"] = "unauthorized";
                            sendJson(req, d, 401); return; }
        bool queued = ctx_.onReset && ctx_.onReset();
        JsonDocument doc;
        doc["ok"] = queued;
        doc["accepted"] = queued;
        // The reset is executed by loop(); it may still be refused there (E-stop /
        // LIMIT active). Poll /api/status for the outcome.
        doc["note"] = queued ? "reset queued; watch /api/status" : "command queue full";
        sendJson(req, doc, queued ? 202 : 503);
    });

    // ---- GET /api/board/{id} ----
    server_->on("/api/board/esp32-s3-devkitc-1", HTTP_GET,
               [](AsyncWebServerRequest* req) {
        const BoardProfile* b = builtinBoardProfile("esp32-s3-devkitc-1");
        JsonDocument doc;
        doc["identifier"] = b->identifier;
        doc["displayName"] = b->displayName;
        JsonArray pins = doc["pins"].to<JsonArray>();
        for (const auto& p : b->pins) {
            JsonObject o = pins.add<JsonObject>();
            o["gpio"] = p.gpio;
            o["preference"] = prefName(p.preference);
            o["reserved"] = p.reserved;
            o["usb"] = p.usb;
            o["strapping"] = p.strapping;
            o["highSpeedOutput"] = p.highSpeedOutput;
            o["adc"] = p.adc;
            o["note"] = p.note;
        }
        sendJson(req, doc);
    });

    // ---- POST /api/pins/auto (auto-assign GPIO for the wizard's draft) ----
    // Reads the wizard's request body so the assignment matches the DRAFT being
    // edited (string count, USB reservation, PCA/OE, LIMIT switches), not the
    // currently-active profile.
    auto* pinsAuto = new AsyncCallbackJsonWebHandler(
        "/api/pins/auto", [this](AsyncWebServerRequest* req, JsonVariant& body) {
            const BoardProfile* b = builtinBoardProfile("esp32-s3-devkitc-1");
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
            // Validated above; the actual activation runs in loop() (motor stop,
            // reconfigure, re-home). Report ACCEPTED, not "done".
            bool queued = ctx_.onActivateProfile && ctx_.onActivateProfile(p);
            doc["ok"] = queued;
            doc["accepted"] = queued;
            doc["note"] = queued ? "activation queued; watch /api/status"
                                 : "command queue full";
            sendJson(req, doc, queued ? 202 : 503);
        });
    putProfile->setMethod(HTTP_PUT);
    server_->addHandler(putProfile);

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
            bool queued = ctx_.onActivateProfile && ctx_.onActivateProfile(p);
            doc["ok"] = queued;
            doc["accepted"] = queued;
            doc["note"] = queued ? "activation queued; watch /api/status"
                                 : "invalid profile or queue full";
            sendJson(req, doc, queued ? 202 : 422);
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
            bool queued = ctx_.onTestNote &&
                      ctx_.onTestNote(body["channel"] | 0, body["note"] | 60,
                                      body["velocity"] | 100, body["durationMs"] | 500);
            doc["ok"] = queued;
            doc["accepted"] = queued;
            // Played by loop() only if Ready; watch /api/status for the result.
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
            bool queued = ctx_.onTestServo && ctx_.onTestServo(idx, active);
            doc["ok"] = queued;
            doc["accepted"] = queued;
            doc["note"] = queued ? "servo test queued" : "command queue full";
            sendJson(req, doc, queued ? 202 : 503);
        });
    testServo->setMethod(HTTP_POST);
    server_->addHandler(testServo);

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
            // Only overwrite a password that was actually provided (an absent or
            // empty field leaves the stored secret unchanged).
            bool hasSta = !body["stationPassword"].isNull() &&
                          std::string(body["stationPassword"] | "").size() > 0;
            bool hasAp = !body["apPassword"].isNull() &&
                         std::string(body["apPassword"] | "").size() > 0;
            ctx_.onSetWifi(hasSta, body["stationPassword"] | "", hasAp,
                           body["apPassword"] | "");
            doc["ok"] = true;
            doc["note"] = "stored; reboot to apply";
            sendJson(req, doc);
        });
    setWifi->setMethod(HTTP_POST);
    server_->addHandler(setWifi);

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
    doc["source"] = "wifiUdp";
    doc["channel"] = e.channel;
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

}  // namespace gmb
