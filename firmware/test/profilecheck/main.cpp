// Round-trip check: every shipped instrument-profiles/*.json must load through
// the REAL firmware parser (ProfileStorage::fromJson), the enum string values
// must be honoured (not silently defaulted), and re-serialising then re-parsing
// must reproduce the same enums. Guards the firmware<->web JSON contract (P0-1).
#include <ArduinoJson.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/core/configuration/Profile.h"
#include "../../src/core/configuration/ProfileValidator.h"
#include "../../src/platform/esp32/ProfileStorage.h"

using namespace gmb;

static int g_fail = 0;
#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) { std::printf("  [FAIL] %s\n", msg); ++g_fail; }      \
    } while (0)

static std::string slurp(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool parse(const std::string& json, Profile& out) {
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) return false;
    return ProfileStorage::fromJson(doc.as<JsonVariantConst>(), out);
}

int main(int argc, char** argv) {
    const char* files[] = {
        "instrument-profiles/ukulele-gcea.json",
        "instrument-profiles/guitar-standard.json",
        "instrument-profiles/bass-4string.json",
        "instrument-profiles/mandolin-gdae.json",
        "instrument-profiles/banjo-5string.json",
    };
    std::string root = argc > 1 ? argv[1] : ".";
    for (const char* rel : files) {
        std::string path = root + "/" + rel;
        std::printf("%s\n", rel);
        std::string json = slurp(path);
        CHECK(!json.empty(), "file is readable");
        Profile p;
        CHECK(parse(json, p), "loads through the firmware parser");

        // Re-serialise and re-parse: the enums must survive a full round trip.
        JsonDocument out;
        ProfileStorage::toJson(p, out);
        std::string reser;
        serializeJson(out, reser);
        Profile p2;
        CHECK(parse(reser, p2), "re-serialised profile re-parses");
        CHECK(p2.midi.velocityCurve == p.midi.velocityCurve, "velocityCurve round-trips");
        CHECK(p2.selector.notePositionPolicy == p.selector.notePositionPolicy,
              "notePositionPolicy round-trips");
        CHECK(p2.selector.missingSelectionPolicy == p.selector.missingSelectionPolicy,
              "missingSelectionPolicy round-trips");
        CHECK(p2.selector.fret.invalidValuePolicy == p.selector.fret.invalidValuePolicy,
              "fret.invalidValuePolicy round-trips");
        CHECK(p2.strings.size() == p.strings.size() &&
                  (p.strings.empty() ||
                   p2.strings[0].transmission == p.strings[0].transmission),
              "transmission round-trips");
    }

    // An unknown enum string must be REJECTED, not silently defaulted.
    {
        std::printf("unknown-enum rejection\n");
        std::string base = slurp(root + "/instrument-profiles/ukulele-gcea.json");
        JsonDocument doc;
        deserializeJson(doc, base);
        doc["midi"]["velocityCurve"] = "banana";
        std::string bad;
        serializeJson(doc, bad);
        Profile p;
        CHECK(!parse(bad, p), "unknown velocityCurve is rejected");
    }

    // ---- P1.12: explicit v1 -> v2 migration ---------------------------------
    // A REAL stored v1 profile (fixtures/profile-v1-ukulele.json) must load, be
    // upgraded to the current version, and lose the no-op staticIp key — without
    // fromJson() accumulating version special-cases.
    {
        std::printf("v1 -> v2 migration\n");
        std::string raw = slurp(root + "/firmware/test/fixtures/profile-v1-ukulele.json");
        CHECK(!raw.empty(), "v1 fixture is present");

        JsonDocument doc;
        CHECK(deserializeJson(doc, raw) == DeserializationError::Ok, "v1 fixture parses");
        CHECK((doc["profileVersion"] | 0) == 1, "fixture really is v1");
        CHECK(doc["network"]["staticIp"].is<bool>(), "fixture carries the v1 staticIp key");

        ProfileStorage::migrate(doc);
        CHECK((doc["profileVersion"] | 0) == kCurrentProfileVersion,
              "migrate() stamps the current profile version");
        CHECK(!doc["network"]["staticIp"].is<bool>(),
              "migrate() drops the no-op staticIp flag");

        // Idempotent: migrating an already-current document changes nothing.
        std::string once;
        serializeJson(doc, once);
        ProfileStorage::migrate(doc);
        std::string twice;
        serializeJson(doc, twice);
        CHECK(once == twice, "migrate() is idempotent");

        // The migrated document still loads and validates as a real profile, and the
        // v2 blocks the v1 file never had come up at their documented defaults.
        Profile p;
        CHECK(ProfileStorage::fromJson(doc.as<JsonVariantConst>(), p),
              "migrated v1 profile parses");
        CHECK(ProfileValidator::isActivatable(p), "migrated v1 profile is activatable");
        CHECK(p.power.maxConcurrentMoves == 3 && p.power.staggerMs == 8,
              "absent power block defaults to the documented governor caps");
        CHECK(p.pluck.muteSource == MuteSource::Auto &&
                  p.pluck.liftEngage == LiftEngage::LowerToPlay,
              "absent pluck block keeps the historical behaviour");
        CHECK(!p.estopNormallyClosed,
              "absent estopNormallyClosed keeps the legacy NO button polarity");
        CHECK(p.instrument.polyphonyMax == 0, "absent polyphonyMax means automatic");
        CHECK(!p.hardware.oePullup && p.hardware.pcaPullups.empty(),
              "absent hardware block declares nothing built");
        for (const auto& sv : p.servos)
            CHECK(sv.i2cBus == 0 && sv.muteUs == 0,
                  "v1 servos default to bus 0 with no mute position");

        // An import goes through migrate() too, so the same file loads via the
        // public import path.
        ProfileStorage store;
        Profile imported;
        CHECK(store.importJson(raw, imported), "importJson migrates + parses a v1 file");
        CHECK(imported.strings.size() == p.strings.size(),
              "import and direct migrate agree");
    }

    // ---- P1.13: split device/instrument slot storage --------------------------
    // Slots on disk are stored SPLIT (device / instrument sections). The wrappers
    // must be lossless, must produce the declared envelope, and must still read a
    // LEGACY flat slot — including a v1 one, which they migrate on the way in.
    {
        std::printf("device/instrument split slot storage\n");
        std::string flat = slurp(root + "/instrument-profiles/guitar-standard.json");
        Profile p;
        CHECK(parse(flat, p), "source profile parses");

        JsonDocument slot;
        ProfileStorage::toSlotJson(p, slot);
        CHECK(std::string(slot["storageFormat"] | "") == "gmb-split-v1",
              "slot carries the split storage marker");
        CHECK(slot["device"].is<JsonObject>() && slot["instrument"].is<JsonObject>(),
              "slot has both sections");
        // The device half owns the machine; the instrument half owns the tune. The
        // axes AND their homing configs are instrument mechanics, so they must NOT
        // be on the device side.
        CHECK(slot["device"]["board"].is<JsonObject>(), "board is device-side");
        CHECK(slot["device"]["pins"].is<JsonArray>(), "pins are device-side");
        CHECK(slot["device"]["network"].is<JsonObject>(), "network is device-side");
        CHECK(slot["device"]["hardware"].is<JsonObject>(), "hardware notes are device-side");
        CHECK(slot["instrument"]["strings"].is<JsonArray>(), "strings are instrument-side");
        CHECK(slot["instrument"]["servos"].is<JsonArray>(), "servos are instrument-side");
        CHECK(slot["instrument"]["strings"][0]["homing"].is<JsonObject>(),
              "homing travels with the instrument, not the device");
        CHECK(!slot["strings"].is<JsonArray>(), "nothing is left at the top level");

        // Lossless round trip through the split form.
        std::string slotStr;
        serializeJson(slot, slotStr);
        JsonDocument reread;
        CHECK(deserializeJson(reread, slotStr) == DeserializationError::Ok,
              "split slot re-parses");
        Profile back;
        CHECK(ProfileStorage::fromSlotJson(reread.as<JsonVariantConst>(), back),
              "split slot loads");
        CHECK(back.strings.size() == p.strings.size(), "string count survives the split");
        CHECK(back.homing.size() == p.homing.size(), "homing count survives the split");
        CHECK(back.servos.size() == p.servos.size(), "servo count survives the split");
        CHECK(back.pins.size() == p.pins.size(), "pin count survives the split");
        CHECK(back.boardIdentifier == p.boardIdentifier, "board identifier survives");
        CHECK(back.instrument.name == p.instrument.name, "instrument name survives");
        CHECK(back.network.hostname == p.network.hostname, "hostname survives");
        CHECK(back.midi.chordWindowMs == p.midi.chordWindowMs, "MIDI config survives");
        CHECK(back.power.staggerMs == p.power.staggerMs, "power config survives");
        CHECK(!back.strings.empty() && !p.strings.empty() &&
                  back.strings[0].maxPositionMm == p.strings[0].maxPositionMm,
              "axis geometry survives");
        CHECK(!back.homing.empty() && !p.homing.empty() &&
                  back.homing[0].offsetMm == p.homing[0].offsetMm,
              "homing offsets survive");

        // A LEGACY flat slot (what older firmware wrote) still loads — no profile is
        // orphaned; it is simply rewritten split on the next save.
        JsonDocument legacy;
        CHECK(deserializeJson(legacy, flat) == DeserializationError::Ok,
              "legacy flat slot parses");
        Profile legacyOut;
        CHECK(ProfileStorage::fromSlotJson(legacy.as<JsonVariantConst>(), legacyOut),
              "legacy FLAT slot still loads through fromSlotJson");
        CHECK(legacyOut.strings.size() == p.strings.size(), "legacy slot keeps its axes");

        // ...and a legacy flat slot that is ALSO v1 is migrated on the way in.
        std::string v1 = slurp(root + "/firmware/test/fixtures/profile-v1-ukulele.json");
        JsonDocument v1doc;
        CHECK(deserializeJson(v1doc, v1) == DeserializationError::Ok, "v1 slot parses");
        Profile v1out;
        CHECK(ProfileStorage::fromSlotJson(v1doc.as<JsonVariantConst>(), v1out),
              "legacy v1 flat slot loads (migrated)");
        CHECK(ProfileValidator::isActivatable(v1out), "migrated legacy slot is activatable");
    }

    std::printf(g_fail ? "\nPROFILECHECK FAILED (%d)\n" : "\nprofilecheck OK\n", g_fail);
    return g_fail ? 1 : 0;
}
