#include "ProfileStorage.h"

#include "../../core/configuration/ProfileValidator.h"

#if defined(ARDUINO)
#include <LittleFS.h>
#include <Preferences.h>
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
Transmission transmissionFrom(JsonVariantConst v, bool* ok) {
    if (v.is<const char*>()) {
        std::string s = v.as<const char*>();
        if (s == "beltGt2") return Transmission::BeltGt2;
        if (s == "screw") return Transmission::Screw;
        if (s == "custom") return Transmission::Custom;
        *ok = false; return Transmission::BeltGt2;  // unknown -> reject the profile
    }
    if (v.is<int>()) { int i = v.as<int>(); if (i < 0 || i > 2) *ok = false;
                       return static_cast<Transmission>(i < 0 ? 0 : i > 2 ? 0 : i); }
    return Transmission::BeltGt2;  // absent: default allowed
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

// ---- String <-> enum for the fields the web interface writes as strings ----
// The canonical on-disk / on-wire form is the STRING name (matching the web UI
// and the shipped example profiles). Parsing also accepts a legacy NUMBER for
// backward compatibility. An unknown STRING sets *ok=false so the whole profile
// is rejected rather than silently mapped to a default (audit P0-1 / P0-7).

const char* velocityCurveName(VelocityCurve c) {
    switch (c) {
        case VelocityCurve::Linear: return "linear";
        case VelocityCurve::Soft: return "soft";
        case VelocityCurve::Hard: return "hard";
        case VelocityCurve::Exponential: return "exponential";
        default: return "custom";
    }
}
VelocityCurve velocityCurveFrom(JsonVariantConst v, bool* ok) {
    if (v.is<const char*>()) {
        std::string s = v.as<const char*>();
        if (s == "linear") return VelocityCurve::Linear;
        if (s == "soft") return VelocityCurve::Soft;
        if (s == "hard") return VelocityCurve::Hard;
        if (s == "exponential") return VelocityCurve::Exponential;
        if (s == "custom") return VelocityCurve::Custom;
        *ok = false; return VelocityCurve::Linear;
    }
    if (v.is<int>()) { int i = v.as<int>(); if (i < 0 || i > 4) *ok = false;
                       return static_cast<VelocityCurve>(i < 0 ? 0 : i > 4 ? 4 : i); }
    return VelocityCurve::Linear;  // absent: default allowed
}

const char* saturationName(SaturationStrategy s) {
    switch (s) {
        case SaturationStrategy::IgnoreExtra: return "ignoreExtra";
        case SaturationStrategy::PriorityLow: return "priorityLow";
        case SaturationStrategy::PriorityHigh: return "priorityHigh";
        case SaturationStrategy::PriorityFirst: return "priorityFirst";
        case SaturationStrategy::ReplaceOldest: return "replaceOldest";
        default: return "monophonic";
    }
}
SaturationStrategy saturationFrom(JsonVariantConst v, bool* ok) {
    if (v.is<const char*>()) {
        std::string s = v.as<const char*>();
        if (s == "ignoreExtra") return SaturationStrategy::IgnoreExtra;
        if (s == "priorityLow") return SaturationStrategy::PriorityLow;
        if (s == "priorityHigh") return SaturationStrategy::PriorityHigh;
        if (s == "priorityFirst") return SaturationStrategy::PriorityFirst;
        if (s == "replaceOldest") return SaturationStrategy::ReplaceOldest;
        if (s == "monophonic") return SaturationStrategy::Monophonic;
        *ok = false; return SaturationStrategy::PriorityLow;
    }
    if (v.is<int>()) { int i = v.as<int>(); if (i < 0 || i > 5) *ok = false;
                       return static_cast<SaturationStrategy>(i < 0 ? 1 : i > 5 ? 1 : i); }
    return SaturationStrategy::PriorityLow;
}

const char* notePolicyName(NotePositionPolicy p) {
    switch (p) {
        case NotePositionPolicy::CcPriorityWithWarning: return "ccPriorityWithWarning";
        case NotePositionPolicy::NotePriority: return "notePriority";
        default: return "strict";
    }
}
NotePositionPolicy notePolicyFrom(JsonVariantConst v, bool* ok) {
    if (v.is<const char*>()) {
        std::string s = v.as<const char*>();
        if (s == "ccPriorityWithWarning") return NotePositionPolicy::CcPriorityWithWarning;
        if (s == "notePriority") return NotePositionPolicy::NotePriority;
        if (s == "strict") return NotePositionPolicy::Strict;
        *ok = false; return NotePositionPolicy::CcPriorityWithWarning;
    }
    if (v.is<int>()) { int i = v.as<int>(); if (i < 0 || i > 2) *ok = false;
                       return static_cast<NotePositionPolicy>(i < 0 ? 0 : i > 2 ? 0 : i); }
    return NotePositionPolicy::CcPriorityWithWarning;
}

// The missing/expired selection policy is an InvalidValuePolicy, but the web only
// exposes {automaticAllocation, reject} and names the fallback "automaticAllocation".
const char* selectionPolicyName(InvalidValuePolicy p) {
    return p == InvalidValuePolicy::Reject ? "reject" : "automaticAllocation";
}
InvalidValuePolicy selectionPolicyFrom(JsonVariantConst v, bool* ok) {
    if (v.is<const char*>()) {
        std::string s = v.as<const char*>();
        if (s == "reject") return InvalidValuePolicy::Reject;
        if (s == "automaticAllocation" || s == "automaticFallback")
            return InvalidValuePolicy::AutomaticFallback;
        if (s == "lastValid") return InvalidValuePolicy::LastValid;
        if (s == "clamp") return InvalidValuePolicy::Clamp;
        *ok = false; return InvalidValuePolicy::AutomaticFallback;
    }
    if (v.is<int>()) { int i = v.as<int>(); if (i < 0 || i > 3) *ok = false;
                       return static_cast<InvalidValuePolicy>(i < 0 ? 2 : i > 3 ? 2 : i); }
    return InvalidValuePolicy::AutomaticFallback;
}

// The fret invalid-value policy exposes all four InvalidValuePolicy values.
const char* fretPolicyName(InvalidValuePolicy p) {
    switch (p) {
        case InvalidValuePolicy::Reject: return "reject";
        case InvalidValuePolicy::Clamp: return "clamp";
        case InvalidValuePolicy::LastValid: return "lastValid";
        default: return "automaticFallback";
    }
}
InvalidValuePolicy fretPolicyFrom(JsonVariantConst v, bool* ok) {
    if (v.is<const char*>()) {
        std::string s = v.as<const char*>();
        if (s == "reject") return InvalidValuePolicy::Reject;
        if (s == "clamp") return InvalidValuePolicy::Clamp;
        if (s == "automaticFallback" || s == "automaticAllocation")
            return InvalidValuePolicy::AutomaticFallback;
        if (s == "lastValid") return InvalidValuePolicy::LastValid;
        *ok = false; return InvalidValuePolicy::AutomaticFallback;
    }
    if (v.is<int>()) { int i = v.as<int>(); if (i < 0 || i > 3) *ok = false;
                       return static_cast<InvalidValuePolicy>(i < 0 ? 2 : i > 3 ? 2 : i); }
    return InvalidValuePolicy::AutomaticFallback;
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
        o["kind"] = static_cast<int>(a.kind);
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
    mi["sustainCc"] = p.midi.sustainCc;
    mi["velocityCurve"] = velocityCurveName(p.midi.velocityCurve);
    mi["saturationStrategy"] = saturationName(p.midi.saturationStrategy);

    JsonObject sf = doc["stringFretSelection"].to<JsonObject>();
    sf["enabled"] = p.selector.enabled;
    sf["mode"] = modeName(p.selector.mode);
    sf["perMidiChannel"] = p.selector.perMidiChannel;
    sf["selectionTimeoutMs"] = p.selector.selectionTimeoutMs;
    sf["prepareOnCompleteSelection"] = p.selector.prepareOnCompleteSelection;
    sf["queueDepth"] = p.selector.queueDepth;
    // Policies live under `validation` as string names, matching the web UI.
    JsonObject val = sf["validation"].to<JsonObject>();
    val["notePositionPolicy"] = notePolicyName(p.selector.notePositionPolicy);
    val["missingSelectionPolicy"] = selectionPolicyName(p.selector.missingSelectionPolicy);
    val["expiredSelectionPolicy"] = selectionPolicyName(p.selector.expiredSelectionPolicy);
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
    fr["invalidValuePolicy"] = fretPolicyName(p.selector.fret.invalidValuePolicy);

    JsonArray strings = doc["strings"].to<JsonArray>();
    for (size_t i = 0; i < p.strings.size(); ++i) {
        const AxisConfig& a = p.strings[i];
        JsonObject o = strings.add<JsonObject>();
        o["enabled"] = a.enabled;
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
            ho["limitActiveHigh"] = h.limitActiveHigh;
        }
    }

    JsonArray servos = doc["servos"].to<JsonArray>();
    for (const auto& s : p.servos) {
        JsonObject o = servos.add<JsonObject>();
        o["enabled"] = s.enabled;
        o["function"] = s.function;
        o["stringIndex"] = s.stringIndex;
        o["source"] = s.source == ServoSource::DirectGpio ? "gpio" : "pca";
        o["pcaBoard"] = s.pcaBoard;
        o["channel"] = s.channel;
        o["gpio"] = s.gpio;
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

bool ProfileStorage::fromJson(JsonVariantConst doc, Profile& out) {
    if (doc["instrument"].isNull()) return false;
    bool enumsOk = true;  // set false by any unknown enum string -> reject profile
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
        // The signal kind is ALWAYS derived from the signal name — never trusted
        // from the JSON. Otherwise a profile could label a "STEP1" pin as Generic
        // to dodge the strict high-speed-output GPIO validation.
        a.kind = signalKindFromName(a.signal);
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
    out.midi.sustainCc = mi["sustainCc"] | 64;
    out.midi.velocityCurve = velocityCurveFrom(mi["velocityCurve"], &enumsOk);
    out.midi.saturationStrategy = saturationFrom(mi["saturationStrategy"], &enumsOk);

    JsonObjectConst sf = doc["stringFretSelection"];
    out.selector.enabled = sf["enabled"] | true;
    out.selector.mode = modeFrom(sf["mode"] | "hybrid");
    out.selector.perMidiChannel = sf["perMidiChannel"] | true;
    out.selector.selectionTimeoutMs = sf["selectionTimeoutMs"] | 100;
    out.selector.prepareOnCompleteSelection = sf["prepareOnCompleteSelection"] | true;
    out.selector.queueDepth = sf["queueDepth"] | 32;
    // Policies live under `validation` (web format); fall back to the legacy flat
    // location so old firmware-exported profiles still load.
    JsonObjectConst val = sf["validation"];
    JsonVariantConst npp = !val["notePositionPolicy"].isNull() ? val["notePositionPolicy"]
                                                               : sf["notePositionPolicy"];
    JsonVariantConst msp = !val["missingSelectionPolicy"].isNull() ? val["missingSelectionPolicy"]
                                                                   : sf["missingSelectionPolicy"];
    JsonVariantConst esp = !val["expiredSelectionPolicy"].isNull() ? val["expiredSelectionPolicy"]
                                                                   : sf["expiredSelectionPolicy"];
    out.selector.notePositionPolicy = notePolicyFrom(npp, &enumsOk);
    out.selector.missingSelectionPolicy = selectionPolicyFrom(msp, &enumsOk);
    out.selector.expiredSelectionPolicy = selectionPolicyFrom(esp, &enumsOk);
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
    out.selector.fret.invalidValuePolicy = fretPolicyFrom(fr["invalidValuePolicy"], &enumsOk);

    out.strings.clear();
    out.homing.clear();
    for (JsonObjectConst o : doc["strings"].as<JsonArrayConst>()) {
        AxisConfig a;
        a.enabled = o["enabled"] | true;
        a.openNote = o["openNote"] | 40;
        a.maxFret = o["maxFret"] | 12;
        a.scaleLengthMm = o["scaleLengthMm"] | 330.0;
        a.transmission = transmissionFrom(o["transmission"], &enumsOk);
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
        h.limitActiveHigh = ho["limitActiveHigh"] | false;
        out.homing.push_back(h);
    }

    out.servos.clear();
    for (JsonObjectConst o : doc["servos"].as<JsonArrayConst>()) {
        ServoConfig s;
        s.enabled = o["enabled"] | false;
        s.function = o["function"] | "finger";
        s.stringIndex = o["stringIndex"] | -1;
        std::string src = o["source"] | "pca";
        s.source = src == "gpio" ? ServoSource::DirectGpio : ServoSource::Pca;
        s.pcaBoard = o["pcaBoard"] | 0;
        s.channel = o["channel"] | 0;
        s.gpio = o["gpio"] | -1;
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
    // Reject the whole profile if any enum string was unknown (never silently map
    // an unrecognised value to a default — audit P0-1 / P0-7).
    return enumsOk;
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
    return fromJson(doc.as<JsonVariantConst>(), out);
}

std::string ProfileStorage::slotPath(int slot) {
    return std::string("/profiles/profile") + std::to_string(slot) + ".json";
}

#if defined(ARDUINO)
bool ProfileStorage::begin() {
    // Try to mount WITHOUT auto-formatting first. A previously-initialised
    // filesystem that now fails to mount is treated as CORRUPT, not first-boot:
    // going degraded (no auto-format) protects the stored profiles from being
    // silently wiped by a transient error (audit P1-3). An NVS marker tells the
    // two cases apart.
    degraded_ = false;
    Preferences prefs;
    prefs.begin("gmb", false);
    bool everInitialised = prefs.getBool("fsinit", false);
    if (LittleFS.begin(false)) {
        if (!everInitialised) prefs.putBool("fsinit", true);
        prefs.end();
    } else if (!everInitialised) {
        // Genuine first boot (or never formatted): formatting is expected.
        prefs.putBool("fsinit", true);
        prefs.end();
        if (!LittleFS.begin(true)) return false;
    } else {
        // Known filesystem that won't mount -> corrupt. Stay degraded; require an
        // explicit format() to recover (never auto-wipe the user's profiles).
        prefs.end();
        degraded_ = true;
        return false;
    }
    if (!LittleFS.exists("/profiles")) LittleFS.mkdir("/profiles");
    // A slot file is "healthy" only if it deserialises to a valid Profile.
    auto healthy = [](const std::string& path) -> bool {
        File f = LittleFS.open(path.c_str(), "r");
        if (!f) return false;
        JsonDocument doc;
        bool ok = deserializeJson(doc, f) == DeserializationError::Ok;
        Profile check;
        ok = ok && fromJson(doc.as<JsonVariantConst>(), check);
        f.close();
        return ok;
    };
    // Recover from an interrupted save: restore the .bak when the final file is
    // MISSING, or present but CORRUPT (truncated / invalid JSON), as long as the
    // .bak itself is healthy (audit P1-4).
    for (int i = 0; i < kMaxProfiles; ++i) {
        std::string path = slotPath(i);
        std::string bak = path + ".bak";
        if (!LittleFS.exists(bak.c_str())) continue;
        bool finalOk = LittleFS.exists(path.c_str()) && healthy(path);
        if (!finalOk && healthy(bak)) {
            LittleFS.remove(path.c_str());
            LittleFS.rename(bak.c_str(), path.c_str());
        }
    }
    return true;
}

bool ProfileStorage::format() {
    // Deliberate wipe-and-reformat (recovery from a corrupt filesystem).
    LittleFS.end();
    if (!LittleFS.begin(true)) return false;
    if (!LittleFS.exists("/profiles")) LittleFS.mkdir("/profiles");
    Preferences prefs;
    prefs.begin("gmb", false);
    prefs.putBool("fsinit", true);
    prefs.end();
    degraded_ = false;
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
    if (slot < 0 || slot >= kMaxProfiles) return false;
    File f = LittleFS.open(slotPath(slot).c_str(), "r");
    if (!f) return false;
    JsonDocument doc;
    bool ok = deserializeJson(doc, f) == DeserializationError::Ok && fromJson(doc.as<JsonVariantConst>(), out);
    f.close();
    return ok;
}

bool ProfileStorage::save(int slot, const Profile& p) {
    if (slot < 0 || slot >= kMaxProfiles) return false;
    // Centralised gate: never persist a semantically invalid profile — it could
    // be chosen as the startup slot and would then be rejected at boot (§21.1).
    if (!ProfileValidator::isActivatable(p)) return false;
    // Write to a temp file first, and only replace the existing slot once the
    // temp file is fully written — so a failure never destroys the old profile.
    std::string finalPath = slotPath(slot);
    std::string tmp = finalPath + ".tmp";
    File f = LittleFS.open(tmp.c_str(), "w");
    if (!f) return false;
    JsonDocument doc;
    toJson(p, doc);
    size_t written = serializeJson(doc, f);
    f.close();
    if (written == 0) {  // write failed: keep the old slot intact
        LittleFS.remove(tmp.c_str());
        return false;
    }
    // Read the temp file back and confirm it deserialises to a valid profile
    // BEFORE replacing the existing slot (guards against a truncated write).
    {
        File rf = LittleFS.open(tmp.c_str(), "r");
        if (!rf) { LittleFS.remove(tmp.c_str()); return false; }
        JsonDocument vd;
        Profile check;
        bool ok = deserializeJson(vd, rf) == DeserializationError::Ok &&
                  fromJson(vd.as<JsonVariantConst>(), check);
        rf.close();
        if (!ok) { LittleFS.remove(tmp.c_str()); return false; }
    }
    // Keep a backup of the existing slot so a failed rename never loses data.
    std::string bak = finalPath + ".bak";
    LittleFS.remove(bak.c_str());
    bool hadOld = LittleFS.exists(finalPath.c_str());
    if (hadOld) LittleFS.rename(finalPath.c_str(), bak.c_str());
    if (LittleFS.rename(tmp.c_str(), finalPath.c_str())) {
        LittleFS.remove(bak.c_str());  // success: drop the backup
        return true;
    }
    // Rename failed: restore the previous profile from the backup.
    if (hadOld) LittleFS.rename(bak.c_str(), finalPath.c_str());
    LittleFS.remove(tmp.c_str());
    return false;
}

bool ProfileStorage::remove(int slot) {
    if (slot < 0 || slot >= kMaxProfiles) return false;
    return LittleFS.remove(slotPath(slot).c_str());
}

int ProfileStorage::startupSlot() const {
    File f = LittleFS.open("/startup.txt", "r");
    if (!f) return 0;
    int slot = f.parseInt();
    f.close();
    if (slot < 0 || slot >= kMaxProfiles) return 0;  // bound the stored value
    return slot;
}

void ProfileStorage::setStartupSlot(int slot) {
    if (slot < 0 || slot >= kMaxProfiles) return;  // never store out of range
    File f = LittleFS.open("/startup.txt", "w");
    if (f) {
        f.print(slot);
        f.close();
    }
}
#else
// Non-Arduino stubs so the file is analysable off-target.
bool ProfileStorage::begin() { return false; }
bool ProfileStorage::format() { return false; }
std::vector<std::string> ProfileStorage::list() const { return {}; }
bool ProfileStorage::load(int, Profile&) const { return false; }
bool ProfileStorage::save(int, const Profile&) { return false; }
bool ProfileStorage::remove(int) { return false; }
int ProfileStorage::startupSlot() const { return 0; }
void ProfileStorage::setStartupSlot(int) {}
#endif

}  // namespace gmb
