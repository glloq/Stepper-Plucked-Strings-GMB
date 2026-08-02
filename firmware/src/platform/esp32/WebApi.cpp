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

}  // namespace

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

void WebApi::registerRoutes() {
    // ---- GET /api/status ----
    server_->on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["wifi"] = ctx_.net ? ctx_.net->mode() : "unknown";
        doc["ip"] = ctx_.net ? ctx_.net->ipAddress() : "";
        doc["profile"] = ctx_.profile ? ctx_.profile->instrument.name : "";
        doc["safety"] = ctx_.safety && ctx_.safety->actuatorsAllowed() ? "armed" : "safe";
        int ready = ctx_.instrument ? static_cast<int>(ctx_.instrument->stringCount()) : 0;
        doc["stringsReady"] = ready;
        doc["notesPlaying"] = ctx_.instrument ? ctx_.instrument->soundingCount() : 0;
        JsonArray strings = doc["strings"].to<JsonArray>();
        if (ctx_.instrument && ctx_.steppers) {
            for (size_t i = 0; i < ctx_.instrument->stringCount(); ++i) {
                JsonObject s = strings.add<JsonObject>();
                s["index"] = i;
                s["fret"] = ctx_.instrument->target(i).fret;
                s["active"] = ctx_.instrument->target(i).active;
                s["positionMm"] = ctx_.steppers->positionMm(i);
                s["targetMm"] = ctx_.instrument->target(i).positionMm;
                s["home"] = ctx_.steppers->homeActive(i);
            }
        }
        sendJson(req, doc);
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

    // ---- POST /api/pins/auto ----
    server_->on("/api/pins/auto", HTTP_POST, [this](AsyncWebServerRequest* req) {
        const BoardProfile* b = builtinBoardProfile("esp32-s3-devkitc-1");
        PinManager pm(*b);
        PinRequest r;
        r.stringCount = ctx_.profile ? ctx_.profile->instrument.stringCount : 4;
        bool ok = pm.autoAssign(r);
        JsonDocument doc;
        doc["ok"] = ok;
        JsonArray pins = doc["pins"].to<JsonArray>();
        for (const auto& a : pm.assignments()) {
            JsonObject o = pins.add<JsonObject>();
            o["signal"] = a.signal;
            o["gpio"] = a.gpio;
        }
        sendJson(req, doc);
    });

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
        if (ctx_.sysex) {
            const CapabilitySnapshot& s = ctx_.sysex->snapshot();
            doc["revision"] = s.revision;
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
        if (ctx_.profile) ProfileStorage::toJson(*ctx_.profile, doc);
        sendJson(req, doc);
    });

    // ---- GET /api/profiles (slot list) ----
    server_->on("/api/profiles", HTTP_GET, [this](AsyncWebServerRequest* req) {
        JsonDocument doc;
        JsonArray arr = doc["profiles"].to<JsonArray>();
        if (ctx_.storage) {
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
            bool applied = ctx_.onActivateProfile && ctx_.onActivateProfile(p);
            doc["ok"] = applied;
            sendJson(req, doc, applied ? 200 : 409);
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
            JsonDocument doc;
            int slot = body["slot"] | -1;
            Profile p;
            bool parsed = body["profile"].is<JsonObject>()
                              ? ProfileStorage::fromJson(body["profile"], p)
                              : (ctx_.profile ? (p = *ctx_.profile, true) : false);
            if (slot < 0 || !parsed || !ctx_.storage) {
                doc["ok"] = false;
                doc["error"] = "slot and profile required";
                sendJson(req, doc, 400);
                return;
            }
            bool saved = ctx_.storage->save(slot, p);
            if (saved && (body["startup"] | false)) ctx_.storage->setStartupSlot(slot);
            doc["ok"] = saved;
            sendJson(req, doc, saved ? 200 : 500);
        });
    saveProfile->setMethod(HTTP_POST);
    server_->addHandler(saveProfile);

    // ---- POST /api/test/note (inject a Note On/Off pair) ----
    auto* testNote = new AsyncCallbackJsonWebHandler(
        "/api/test/note", [this](AsyncWebServerRequest* req, JsonVariant& body) {
            JsonDocument doc;
            if (!ctx_.instrument) {
                doc["ok"] = false;
                sendJson(req, doc, 409);
                return;
            }
            MidiEvent e;
            e.type = static_cast<uint8_t>(MidiType::NoteOn);
            e.channel = body["channel"] | 0;
            e.data1 = body["note"] | 60;
            e.data2 = body["velocity"] | 100;
            e.timestampUs = micros();
            e.source = static_cast<uint8_t>(MidiSource::WebUiTest);
            ctx_.instrument->handleEvent(e, e.timestampUs);
            doc["ok"] = true;
            sendJson(req, doc);
        });
    testNote->setMethod(HTTP_POST);
    server_->addHandler(testNote);

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
            auto out = ctx_.sysex->handleMessage(in.data(), in.size(), millis());
            doc["ok"] = true;
            JsonArray resp = doc["response"].to<JsonArray>();
            for (uint8_t b : out) resp.add(b);
            sendJson(req, doc);
        });
    sysexReq->setMethod(HTTP_POST);
    server_->addHandler(sysexReq);
}

void WebApi::broadcastStatus() {
    if (statusWs_.count() == 0) return;
    JsonDocument doc;
    doc["notesPlaying"] = ctx_.instrument ? ctx_.instrument->soundingCount() : 0;
    doc["safety"] = ctx_.safety && ctx_.safety->actuatorsAllowed() ? "armed" : "safe";
    String out;
    serializeJson(doc, out);
    statusWs_.textAll(out);
}

#else  // non-Arduino: no-op so the file is analysable off-target.

void WebApi::begin(const WebContext& ctx, uint16_t) { ctx_ = ctx; }
void WebApi::broadcastStatus() {}

#endif

}  // namespace gmb
