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

    std::printf(g_fail ? "\nPROFILECHECK FAILED (%d)\n" : "\nprofilecheck OK\n", g_fail);
    return g_fail ? 1 : 0;
}
