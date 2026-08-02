#include "WebApi.h"

#include "ProfileStorage.h"

#if defined(ARDUINO)
#include <ArduinoJson.h>
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
    // Body handlers (PUT /api/profile, POST /api/pins/validate, /api/sysex/request,
    // /api/test/note) are registered with AsyncCallbackJsonWebHandler in main.cpp
    // where the JSON body dependency is available.
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
