// Emit board-profiles/*.json from the C++ board profiles — the single source of
// truth (audit P1.14 / P1.15).
//
// The shipped JSON board profiles are what the web wizard reads to colour and
// filter pins; BoardProfile.cpp is what the firmware actually validates against.
// Two hand-maintained copies of the same table WILL drift — and a drifted copy is
// exactly the failure that lets the wizard offer a pin the validator then refuses
// (or worse, that the firmware drives while the board uses it for something else).
//
// So this tool renders the JSON straight from the C++ tables. `run.sh --check`
// regenerates into a temp dir and diffs; CI fails if a board profile was edited on
// only one side.
//
// Pure C++17 (no Arduino): it links the same core BoardProfile.cpp the firmware does.
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../src/core/board/BoardProfile.h"

using namespace gmb;

namespace {

const char* preferenceName(PinPreference p) {
    switch (p) {
        case PinPreference::Recommended: return "recommended";
        case PinPreference::Caution:     return "caution";
        case PinPreference::Reserved:    return "reserved";
        case PinPreference::Used:        return "used";
    }
    return "caution";
}

std::string jsonEscape(const std::string& in) {
    std::string out;
    for (char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
        }
    }
    return out;
}

void writeBool(std::string& s, const char* key, bool v, int indent) {
    s += std::string(indent, ' ') + "\"" + key + "\": " + (v ? "true" : "false") + ",\n";
}

// The recommended-assignment block the web wizard reads. It mirrors
// PinManager::autoAssign for the reference board; the classic ESP32 boards have a
// smaller usable set, so their tables are shorter (and a 6-axis instrument simply
// will not auto-assign there — the validator says so explicitly).
struct Recommendation {
    std::vector<int> step, dir, home;
    int sda, scl, enable, servoOe;
};

Recommendation recommendationFor(const std::string& id) {
    if (id == "esp32-s3-devkitc-1" || id == "esp32-s3-devkitc-1-v1.1")
        return {{4, 5, 6, 7, 15, 16}, {17, 18, 8, 9, 10, 11}, {12, 13, 14, 21, 38, 39},
                40, 41, 42, 47};
    // Classic ESP32: only 8 comfortable high-speed outputs remain once UART0, the
    // flash pads, the strapping pins and the input-only pins are excluded.
    return {{4, 13, 14, 25}, {26, 27, 32, 33}, {16, 17, 18, 19}, 21, 22, 23, 5};
}

std::string intList(const std::vector<int>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        s += std::to_string(v[i]);
        if (i + 1 < v.size()) s += ", ";
    }
    return s + "]";
}

std::string recommendedAssignment(const std::string& id) {
    const Recommendation r = recommendationFor(id);
    std::string s = "  \"recommendedAssignment\": {\n";
    s += "    \"STEP\": " + intList(r.step) + ",\n";
    s += "    \"DIR\": " + intList(r.dir) + ",\n";
    s += "    \"HOME\": " + intList(r.home) + ",\n";
    s += "    \"SDA\": " + std::to_string(r.sda) + ",\n";
    s += "    \"SCL\": " + std::to_string(r.scl) + ",\n";
    s += "    \"ENABLE\": " + std::to_string(r.enable) + ",\n";
    s += "    \"SERVO_OE\": " + std::to_string(r.servoOe) + "\n";
    s += "  },\n";
    return s;
}

std::string describe(const std::string& id) {
    if (id == "esp32-s3-devkitc-1")
        return "GPIO capability map for the Espressif ESP32-S3-DevKitC-1 v1.0 "
               "(RGB LED on GPIO48), mirroring firmware/src/core/board/BoardProfile.cpp "
               "(makeEsp32S3DevKitC1). GPIO22-25 do not exist on the ESP32-S3 and are "
               "omitted. GPIO0 is reserved: it is the BOOT button that forces the Wi-Fi "
               "hotspot.";
    if (id == "esp32-s3-devkitc-1-v1.1")
        return "GPIO capability map for the Espressif ESP32-S3-DevKitC-1 v1.1 "
               "(RGB LED moved to GPIO38, so GPIO48 is free), mirroring "
               "firmware/src/core/board/BoardProfile.cpp (makeEsp32S3DevKitC1V11). "
               "Check the silkscreen to tell the two revisions apart.";
    if (id == "esp32-wroom-32")
        return "GPIO capability map for the classic ESP32-WROOM-32 on a 38-pin DevKitC "
               "board, mirroring firmware/src/core/board/BoardProfile.cpp "
               "(makeEsp32Wroom32). Input-only pins 34/35/36/39 cannot output; "
               "GPIO 20/24/28-31 do not exist; 6-11 are the SPI flash. Fewer RMT/MCPWM "
               "units than the S3, so it suits 1-3 axes.";
    return "GPIO capability map for the DOIT ESP32 DevKit v1 (30-pin), mirroring "
           "firmware/src/core/board/BoardProfile.cpp (makeEsp32DevKitV1). Same die as "
           "the WROOM-32 board but the SPI-flash pads (6-11) are not broken out.";
}

std::string render(const BoardProfile& b) {
    std::string s;
    s += "{\n";
    s += "  \"identifier\": \"" + jsonEscape(b.identifier) + "\",\n";
    s += "  \"displayName\": \"" + jsonEscape(b.displayName) + "\",\n";
    s += "  \"description\": \"" + jsonEscape(describe(b.identifier)) + "\",\n";
    s += "  \"reference\": \"SPECIFICATION.md sections 11.4 / 11.5\",\n";
    s += "  \"generatedBy\": \"firmware/tools/dump_board_profiles.cpp — do not edit by "
         "hand; edit firmware/src/core/board/BoardProfile.cpp and regenerate\",\n";
    s += recommendedAssignment(b.identifier);
    s += "  \"pins\": [\n";
    for (size_t i = 0; i < b.pins.size(); ++i) {
        const PinCapability& p = b.pins[i];
        s += "    {\n";
        s += "      \"gpio\": " + std::to_string(static_cast<int>(p.gpio)) + ",\n";
        writeBool(s, "exposed", p.exposed, 6);
        writeBool(s, "input", p.input, 6);
        writeBool(s, "output", p.output, 6);
        writeBool(s, "interrupt", p.interrupt, 6);
        writeBool(s, "internalPullUp", p.internalPullUp, 6);
        writeBool(s, "internalPullDown", p.internalPullDown, 6);
        writeBool(s, "highSpeedOutput", p.highSpeedOutput, 6);
        writeBool(s, "adc", p.adc, 6);
        writeBool(s, "reserved", p.reserved, 6);
        writeBool(s, "strapping", p.strapping, 6);
        writeBool(s, "usb", p.usb, 6);
        writeBool(s, "onboardPeripheral", p.onboardPeripheral, 6);
        s += "      \"preference\": \"" + std::string(preferenceName(p.preference)) + "\",\n";
        s += "      \"note\": \"" + jsonEscape(p.note) + "\"\n";
        s += std::string("    }") + (i + 1 < b.pins.size() ? "," : "") + "\n";
    }
    s += "  ]\n";
    s += "}\n";
    return s;
}

// The same tables again, as a JS module the offline web interface loads. The mock
// backend used to hand-build ONE board in api.js — a third copy of this table, and
// the reason the board picker offered a single model while the firmware supported
// four. Generated here, it cannot drift and the demo covers every board.
std::string renderJs(const std::vector<BoardProfile>& boards) {
    std::string s;
    s += "/*\n";
    s += " * boarddata.js — GENERATED, do not edit by hand.\n";
    s += " *\n";
    s += " * Board capability tables for the offline mock backend, rendered from\n";
    s += " * firmware/src/core/board/BoardProfile.cpp by\n";
    s += " * firmware/tools/dump_board_profiles.cpp. Edit the C++ tables and run\n";
    s += " * firmware/test/boardcheck/run.sh; CI checks this file with --check.\n";
    s += " *\n";
    s += " * On a real device these come from GET /api/boards and /api/board/{id};\n";
    s += " * this file is what makes the file:// demo behave the same.\n";
    s += " */\n";
    s += "(function (global) {\n";
    s += "  'use strict';\n";
    s += "  var GMB = global.GMB = global.GMB || {};\n";
    s += "  GMB.BOARD_PROFILES = [\n";
    for (size_t i = 0; i < boards.size(); ++i) {
        const BoardProfile& b = boards[i];
        s += "    {\n";
        const Recommendation r = recommendationFor(b.identifier);
        s += "      identifier: \"" + jsonEscape(b.identifier) + "\",\n";
        s += "      displayName: \"" + jsonEscape(b.displayName) + "\",\n";
        s += "      recommendedAssignment: {\n";
        s += "        STEP: " + intList(r.step) + ",\n";
        s += "        DIR: " + intList(r.dir) + ",\n";
        s += "        HOME: " + intList(r.home) + ",\n";
        s += "        SDA: " + std::to_string(r.sda) + ", SCL: " + std::to_string(r.scl) +
             ", ENABLE: " + std::to_string(r.enable) +
             ", SERVO_OE: " + std::to_string(r.servoOe) + "\n";
        s += "      },\n";
        s += "      pins: [\n";
        for (size_t j = 0; j < b.pins.size(); ++j) {
            const PinCapability& p = b.pins[j];
            s += "        { gpio: " + std::to_string(static_cast<int>(p.gpio));
            s += ", exposed: " + std::string(p.exposed ? "true" : "false");
            s += ", input: " + std::string(p.input ? "true" : "false");
            s += ", output: " + std::string(p.output ? "true" : "false");
            s += ", interrupt: " + std::string(p.interrupt ? "true" : "false");
            s += ", internalPullUp: " + std::string(p.internalPullUp ? "true" : "false");
            s += ", internalPullDown: " + std::string(p.internalPullDown ? "true" : "false");
            s += ", highSpeedOutput: " + std::string(p.highSpeedOutput ? "true" : "false");
            s += ", adc: " + std::string(p.adc ? "true" : "false");
            s += ", reserved: " + std::string(p.reserved ? "true" : "false");
            s += ", strapping: " + std::string(p.strapping ? "true" : "false");
            s += ", usb: " + std::string(p.usb ? "true" : "false");
            s += ", onboardPeripheral: " + std::string(p.onboardPeripheral ? "true" : "false");
            s += ", preference: \"" + std::string(preferenceName(p.preference)) + "\"";
            s += ", note: \"" + jsonEscape(p.note) + "\" }";
            s += std::string(j + 1 < b.pins.size() ? "," : "") + "\n";
        }
        s += "      ]\n";
        s += std::string("    }") + (i + 1 < boards.size() ? "," : "") + "\n";
    }
    s += "  ];\n";
    s += "})(window);\n";
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: dump_board_profiles <output-directory> [boarddata.js]\n";
        return 2;
    }
    const std::string dir = argv[1];
    const std::vector<BoardProfile> boards = {
        makeEsp32S3DevKitC1(), makeEsp32S3DevKitC1V11(), makeEsp32Wroom32(),
        makeEsp32DevKitV1()};
    for (const BoardProfile& b : boards) {
        const std::string path = dir + "/" + b.identifier + ".json";
        std::ofstream f(path);
        if (!f) {
            std::cerr << "cannot write " << path << "\n";
            return 1;
        }
        f << render(b);
        std::cout << "wrote " << path << "\n";
    }
    if (argc >= 3) {
        std::ofstream f(argv[2]);
        if (!f) {
            std::cerr << "cannot write " << argv[2] << "\n";
            return 1;
        }
        f << renderJs(boards);
        std::cout << "wrote " << argv[2] << "\n";
    }
    return 0;
}
