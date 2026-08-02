#include "ProfileStorage.h"

#if defined(ARDUINO)
#include <LittleFS.h>
#endif

namespace gmb {

namespace {
const char* transmissionName(Transmission t) {
    switch (t) {
        case Transmission::BeltGt2: return "beltGt2";
        case Transmission::Screw: return "screw";
        default: return "custom";
    }
}
Transmission transmissionFrom(const char* s) {
    std::string v = s ? s : "";
    if (v == "screw") return Transmission::Screw;
    if (v == "custom") return Transmission::Custom;
    return Transmission::BeltGt2;
}
const char* modeName(SelectionMode m) {
    switch (m) {
        case SelectionMode::Automatic: return "automatic";
        case SelectionMode::Explicit: return "explicit";
        default: return "hybrid";
    }
}
SelectionMode modeFrom(const char* s) {
    std::string v = s ? s : "";
    if (v == "automatic") return SelectionMode::Automatic;
    if (v == "explicit") return SelectionMode::Explicit;
    return SelectionMode::Hybrid;
}
}  // namespace

void ProfileStorage::toJson(const Profile& p, JsonDocument& doc) {
    doc["project"] = p.project;
    doc["profileVersion"] = p.profileVersion;
    doc["capabilitiesRevision"] = p.capabilitiesRevision;

    JsonObject in = doc["instrument"].to<JsonObject>();
    in["name"] = p.instrument.name;
    in["description"] = p.instrument.description;
    in["stringCount"] = p.instrument.stringCount;
    in["type"] = p.instrument.type;
    in["gmProgram"] = p.instrument.gmProgram;
    in["typeId"] = p.instrument.typeId;
    in["capo"] = p.instrument.capo;
    in["transpose"] = p.instrument.transpose;
    in["pluckMode"] = p.instrument.pluckMode == PluckMode::Individual ? "individual"
                      : p.instrument.pluckMode == PluckMode::SharedStrum ? "sharedStrum"
                                                                         : "both";

    JsonObject bo = doc["board"].to<JsonObject>();
    bo["profile"] = p.boardIdentifier;
    bo["reserveUsb"] = p.reserveUsb;
    bo["automaticPinAssignment"] = p.automaticPinAssignment;

    JsonArray pins = doc["pins"].to<JsonArray>();
    for (const auto& a : p.pins) {
        JsonObject o = pins.add<JsonObject>();
        o["signal"] = a.signal;
        o["gpio"] = a.gpio;
    }

    JsonObject net = doc["network"].to<JsonObject>();
    net["mode"] = p.network.mode == NetworkMode::Station ? "station" : "accessPoint";
    net["ssid"] = p.network.ssid;
    net["hostname"] = p.network.hostname;
    net["apSsid"] = p.network.apSsid;
    net["staticIp"] = p.network.staticIp;

    JsonObject mi = doc["midi"].to<JsonObject>();
    mi["globalChannel"] = p.midi.globalChannel;
    mi["omni"] = p.midi.omni;
    mi["transpose"] = p.midi.transpose;
    mi["chordWindowMs"] = p.midi.chordWindowMs;
    mi["sustainPedal"] = p.midi.sustainPedal;

    JsonObject sf = doc["stringFretSelection"].to<JsonObject>();
    sf["enabled"] = p.selector.enabled;
    sf["mode"] = modeName(p.selector.mode);
    sf["perMidiChannel"] = p.selector.perMidiChannel;
    sf["selectionTimeoutMs"] = p.selector.selectionTimeoutMs;
    sf["prepareOnCompleteSelection"] = p.selector.prepareOnCompleteSelection;
    sf["queueDepth"] = p.selector.queueDepth;
    JsonObject ss = sf["string"].to<JsonObject>();
    ss["ccNumber"] = p.selector.string.ccNumber;
    ss["minimum"] = p.selector.string.minimum;
    ss["maximum"] = p.selector.string.maximum;
    ss["offset"] = p.selector.string.offset;
    ss["numbering"] =
        p.selector.string.numbering == StringNumbering::OneBased ? "oneBased" : "zeroBased";
    ss["reverseOrder"] = p.selector.string.reverseOrder;
    JsonArray map = ss["mapping"].to<JsonArray>();
    for (int8_t m : p.selector.string.mapping) map.add(m);
    JsonObject fr = sf["fret"].to<JsonObject>();
    fr["ccNumber"] = p.selector.fret.ccNumber;
    fr["minimum"] = p.selector.fret.minimum;
    fr["maximum"] = p.selector.fret.maximum;
    fr["offset"] = p.selector.fret.offset;

    JsonArray strings = doc["strings"].to<JsonArray>();
    for (size_t i = 0; i < p.strings.size(); ++i) {
        const AxisConfig& a = p.strings[i];
        JsonObject o = strings.add<JsonObject>();
        o["openNote"] = a.openNote;
        o["maxFret"] = a.maxFret;
        o["scaleLengthMm"] = a.scaleLengthMm;
        o["transmission"] = transmissionName(a.transmission);
        o["stepsPerRevolution"] = a.stepsPerRevolution;
        o["microsteps"] = a.microsteps;
        o["pulleyTeeth"] = a.pulleyTeeth;
        o["beltPitchMm"] = a.beltPitchMm;
        o["leadPerRevolutionMm"] = a.leadPerRevolutionMm;
        o["customStepsPerMm"] = a.customStepsPerMm;
        o["invertDirection"] = a.invertDirection;
        o["minPositionMm"] = a.minPositionMm;
        o["maxPositionMm"] = a.maxPositionMm;
        o["maxSpeedMmS"] = a.maxSpeedMmS;
        o["maxAccelMmS2"] = a.maxAccelMmS2;
        JsonArray cal = o["calibratedFretMm"].to<JsonArray>();
        for (double v : a.calibratedFretMm) cal.add(v);
        if (i < p.homing.size()) {
            const HomingConfig& h = p.homing[i];
            JsonObject ho = o["homing"].to<JsonObject>();
            ho["direction"] = h.direction;
            ho["fastSpeedMmS"] = h.fastSpeedMmS;
            ho["slowSpeedMmS"] = h.slowSpeedMmS;
            ho["backoffMm"] = h.backoffMm;
            ho["offsetMm"] = h.offsetMm;
            ho["timeoutMs"] = h.timeoutMs;
            ho["maxSearchMm"] = h.maxSearchMm;
            ho["sensorActiveHigh"] = h.sensorActiveHigh;
        }
    }

    JsonArray servos = doc["servos"].to<JsonArray>();
    for (const auto& s : p.servos) {
        JsonObject o = servos.add<JsonObject>();
        o["enabled"] = s.enabled;
        o["channel"] = s.channel;
        o["function"] = s.function;
        o["pulseMinUs"] = s.pulseMinUs;
        o["pulseMaxUs"] = s.pulseMaxUs;
        o["restUs"] = s.restUs;
        o["activeUs"] = s.activeUs;
        o["inverted"] = s.inverted;
        o["travelMs"] = s.travelMs;
        o["settleMs"] = s.settleMs;
        o["disableAtRest"] = s.disableAtRest;
    }
}

bool ProfileStorage::fromJson(const JsonDocument& doc, Profile& out) {
    if (doc["instrument"].isNull()) return false;
    out.project = doc["project"] | "Stepper-Plucked-Strings-GMB";
    out.profileVersion = doc["profileVersion"] | 1;
    out.capabilitiesRevision = doc["capabilitiesRevision"] | 1;

    JsonObjectConst in = doc["instrument"];
    out.instrument.name = in["name"] | "Instrument";
    out.instrument.description = in["description"] | "";
    out.instrument.stringCount = in["stringCount"] | 4;
    out.instrument.type = in["type"] | "guitar";
    out.instrument.gmProgram = in["gmProgram"] | 24;
    out.instrument.typeId = in["typeId"] | 4;
    out.instrument.capo = in["capo"] | 0;
    out.instrument.transpose = in["transpose"] | 0;
    std::string pm = in["pluckMode"] | "individual";
    out.instrument.pluckMode = pm == "sharedStrum" ? PluckMode::SharedStrum
                               : pm == "both"       ? PluckMode::Both
                                                    : PluckMode::Individual;

    JsonObjectConst bo = doc["board"];
    out.boardIdentifier = bo["profile"] | "esp32-s3-devkitc-1";
    out.reserveUsb = bo["reserveUsb"] | true;
    out.automaticPinAssignment = bo["automaticPinAssignment"] | true;

    out.pins.clear();
    for (JsonObjectConst o : doc["pins"].as<JsonArrayConst>()) {
        PinAssignment a;
        a.signal = o["signal"] | "";
        a.gpio = o["gpio"] | -1;
        out.pins.push_back(a);
    }

    JsonObjectConst net = doc["network"];
    std::string nm = net["mode"] | "accessPoint";
    out.network.mode = nm == "station" ? NetworkMode::Station : NetworkMode::AccessPoint;
    out.network.ssid = net["ssid"] | "";
    out.network.hostname = net["hostname"] | "gmb-instrument";
    out.network.apSsid = net["apSsid"] | "Stepper-Plucked-Strings-GMB";
    out.network.staticIp = net["staticIp"] | false;

    JsonObjectConst mi = doc["midi"];
    out.midi.globalChannel = mi["globalChannel"] | 0;
    out.midi.omni = mi["omni"] | false;
    out.midi.transpose = mi["transpose"] | 0;
    out.midi.chordWindowMs = mi["chordWindowMs"] | 3;
    out.midi.sustainPedal = mi["sustainPedal"] | true;

    JsonObjectConst sf = doc["stringFretSelection"];
    out.selector.enabled = sf["enabled"] | true;
    out.selector.mode = modeFrom(sf["mode"] | "hybrid");
    out.selector.perMidiChannel = sf["perMidiChannel"] | true;
    out.selector.selectionTimeoutMs = sf["selectionTimeoutMs"] | 100;
    out.selector.prepareOnCompleteSelection = sf["prepareOnCompleteSelection"] | true;
    out.selector.queueDepth = sf["queueDepth"] | 32;
    JsonObjectConst ss = sf["string"];
    out.selector.string.ccNumber = ss["ccNumber"] | 20;
    out.selector.string.minimum = ss["minimum"] | 1;
    out.selector.string.maximum = ss["maximum"] | out.instrument.stringCount;
    out.selector.string.offset = ss["offset"] | 0;
    std::string nb = ss["numbering"] | "oneBased";
    out.selector.string.numbering =
        nb == "zeroBased" ? StringNumbering::ZeroBased : StringNumbering::OneBased;
    out.selector.string.reverseOrder = ss["reverseOrder"] | false;
    out.selector.string.mapping.clear();
    for (JsonVariantConst v : ss["mapping"].as<JsonArrayConst>())
        out.selector.string.mapping.push_back(v.as<int8_t>());
    JsonObjectConst fr = sf["fret"];
    out.selector.fret.ccNumber = fr["ccNumber"] | 21;
    out.selector.fret.minimum = fr["minimum"] | 0;
    out.selector.fret.maximum = fr["maximum"] | 12;
    out.selector.fret.offset = fr["offset"] | 0;

    out.strings.clear();
    out.homing.clear();
    for (JsonObjectConst o : doc["strings"].as<JsonArrayConst>()) {
        AxisConfig a;
        a.openNote = o["openNote"] | 40;
        a.maxFret = o["maxFret"] | 12;
        a.scaleLengthMm = o["scaleLengthMm"] | 330.0;
        a.transmission = transmissionFrom(o["transmission"] | "beltGt2");
        a.stepsPerRevolution = o["stepsPerRevolution"] | 200;
        a.microsteps = o["microsteps"] | 16;
        a.pulleyTeeth = o["pulleyTeeth"] | 20;
        a.beltPitchMm = o["beltPitchMm"] | 2.0;
        a.leadPerRevolutionMm = o["leadPerRevolutionMm"] | 8.0;
        a.customStepsPerMm = o["customStepsPerMm"] | 80.0;
        a.invertDirection = o["invertDirection"] | false;
        a.minPositionMm = o["minPositionMm"] | 0.0;
        a.maxPositionMm = o["maxPositionMm"] | 400.0;
        a.maxSpeedMmS = o["maxSpeedMmS"] | 200.0;
        a.maxAccelMmS2 = o["maxAccelMmS2"] | 2000.0;
        a.calibratedFretMm.clear();
        for (JsonVariantConst v : o["calibratedFretMm"].as<JsonArrayConst>())
            a.calibratedFretMm.push_back(v.as<double>());
        out.strings.push_back(a);

        HomingConfig h;
        JsonObjectConst ho = o["homing"];
        h.direction = ho["direction"] | -1;
        h.fastSpeedMmS = ho["fastSpeedMmS"] | 40.0;
        h.slowSpeedMmS = ho["slowSpeedMmS"] | 5.0;
        h.backoffMm = ho["backoffMm"] | 3.0;
        h.offsetMm = ho["offsetMm"] | 0.0;
        h.timeoutMs = ho["timeoutMs"] | 8000;
        h.maxSearchMm = ho["maxSearchMm"] | 500.0;
        h.sensorActiveHigh = ho["sensorActiveHigh"] | true;
        out.homing.push_back(h);
    }

    out.servos.clear();
    for (JsonObjectConst o : doc["servos"].as<JsonArrayConst>()) {
        ServoConfig s;
        s.enabled = o["enabled"] | false;
        s.channel = o["channel"] | 0;
        s.function = o["function"] | "finger";
        s.pulseMinUs = o["pulseMinUs"] | 500;
        s.pulseMaxUs = o["pulseMaxUs"] | 2500;
        s.restUs = o["restUs"] | 1000;
        s.activeUs = o["activeUs"] | 1800;
        s.inverted = o["inverted"] | false;
        s.travelMs = o["travelMs"] | 120;
        s.settleMs = o["settleMs"] | 30;
        s.disableAtRest = o["disableAtRest"] | true;
        out.servos.push_back(s);
    }
    return true;
}

std::string ProfileStorage::exportJson(const Profile& p, bool /*includeSecrets*/) const {
    JsonDocument doc;
    toJson(p, doc);
    // Ordinary exports never contain the Wi-Fi password (cahier des charges §20);
    // the password is not part of the in-memory Profile at all here.
    std::string out;
    serializeJsonPretty(doc, out);
    return out;
}

bool ProfileStorage::importJson(const std::string& json, Profile& out) const {
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) return false;
    return fromJson(doc, out);
}

std::string ProfileStorage::slotPath(int slot) {
    return std::string("/profiles/profile") + std::to_string(slot) + ".json";
}

#if defined(ARDUINO)
bool ProfileStorage::begin() {
    if (!LittleFS.begin(true)) return false;
    if (!LittleFS.exists("/profiles")) LittleFS.mkdir("/profiles");
    return true;
}

std::vector<std::string> ProfileStorage::list() const {
    std::vector<std::string> names;
    for (int i = 0; i < kMaxProfiles; ++i) {
        File f = LittleFS.open(slotPath(i).c_str(), "r");
        if (f) {
            JsonDocument doc;
            if (deserializeJson(doc, f) == DeserializationError::Ok)
                names.push_back(std::string(doc["instrument"]["name"] | "Profile"));
            else
                names.push_back("");
            f.close();
        } else {
            names.push_back("");
        }
    }
    return names;
}

bool ProfileStorage::load(int slot, Profile& out) const {
    File f = LittleFS.open(slotPath(slot).c_str(), "r");
    if (!f) return false;
    JsonDocument doc;
    bool ok = deserializeJson(doc, f) == DeserializationError::Ok && fromJson(doc, out);
    f.close();
    return ok;
}

bool ProfileStorage::save(int slot, const Profile& p) {
    // Atomic write: write to a temp file then rename (cahier des charges §15).
    std::string tmp = slotPath(slot) + ".tmp";
    File f = LittleFS.open(tmp.c_str(), "w");
    if (!f) return false;
    JsonDocument doc;
    toJson(p, doc);
    serializeJson(doc, f);
    f.close();
    LittleFS.remove(slotPath(slot).c_str());
    return LittleFS.rename(tmp.c_str(), slotPath(slot).c_str());
}

bool ProfileStorage::remove(int slot) {
    return LittleFS.remove(slotPath(slot).c_str());
}

int ProfileStorage::startupSlot() const {
    File f = LittleFS.open("/startup.txt", "r");
    if (!f) return 0;
    int slot = f.parseInt();
    f.close();
    return slot;
}

void ProfileStorage::setStartupSlot(int slot) {
    File f = LittleFS.open("/startup.txt", "w");
    if (f) {
        f.print(slot);
        f.close();
    }
}
#else
// Non-Arduino stubs so the file is analysable off-target.
bool ProfileStorage::begin() { return false; }
std::vector<std::string> ProfileStorage::list() const { return {}; }
bool ProfileStorage::load(int, Profile&) const { return false; }
bool ProfileStorage::save(int, const Profile&) { return false; }
bool ProfileStorage::remove(int) { return false; }
int ProfileStorage::startupSlot() const { return 0; }
void ProfileStorage::setStartupSlot(int) {}
#endif

}  // namespace gmb
