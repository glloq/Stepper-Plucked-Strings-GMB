// Capability snapshot for General-Midi-Boop (spec "Communication ... SysEx").
//
// An immutable snapshot generated from the active profile. A SysEx response
// always uses one snapshot so a config change mid-transmission can never mix two
// versions of the profile (spec section 19).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gmb {

struct Profile;  // forward decl

struct DeviceIdentity {
    uint8_t version = 1;
    uint8_t deviceId[5] = {0, 0, 0, 0, 0};
    std::string deviceName;      // truncated to 32 on the wire (v1); descriptor name (v2)
    std::string model;           // GMB v2 descriptor `device.model` (project name)
    uint8_t firmware[3] = {1, 0, 0};
    uint8_t features = 0x30;     // INSTRUMENT_CAPABILITIES | STRING_CONFIG
};

struct InstrumentDescriptor {
    uint8_t channel = 0;
    uint8_t gmProgram = 24;
    uint8_t typeId = 0x04;
    std::string type;            // GMB v2 descriptor `type` key (e.g. "guitar")
};

struct InstrumentCapabilities {
    uint8_t version = 1;
    uint8_t channel = 0;
    uint8_t gmProgram = 24;
    uint8_t typeId = 0x04;
    uint8_t subType = 0;
    uint8_t noteMode = 0;        // 0 = continuous range, 1 = discrete list
    uint8_t noteMin = 0;
    uint8_t noteMax = 0;
    uint8_t polyphony = 1;
    std::vector<uint8_t> discreteNotes;
    std::vector<uint8_t> supportedCc;
    std::string name;
};

struct StringInstrumentConfig {
    uint8_t version = 1;
    uint8_t channel = 0;
    uint8_t stringCount = 0;
    uint8_t fretCount = 0;
    uint8_t isFretless = 0;      // always 0 for this project
    uint8_t capo = 0;
    uint8_t ccActive = 1;
    uint8_t ccString = 20;
    uint8_t ccFret = 21;
    std::vector<uint8_t> tuning;         // low -> high (GMB order)
    // v2 extras.
    uint8_t ccStringMin = 1, ccStringMax = 6;
    int8_t ccStringOffset = 0;
    uint8_t ccFretMin = 0, ccFretMax = 24;
    int8_t ccFretOffset = 0;
    uint8_t selectionMode = 2;   // 0 auto / 1 explicit / 2 hybrid
    uint8_t stringOrder = 0;     // 0 normal / 1 reversed / 2 custom
    std::vector<uint8_t> fretsPerString;
    std::vector<uint8_t> mapping;
};

struct CapabilitySnapshot {
    uint32_t revision = 0;
    DeviceIdentity identity;
    InstrumentDescriptor descriptor;
    InstrumentCapabilities capabilities;
    StringInstrumentConfig stringConfig;
    uint32_t generatedAtMs = 0;
    bool valid = false;
};

// Build a snapshot from the active profile. `polyphonyOverride` < 0 means
// "automatic" (= number of active strings).
CapabilitySnapshot buildSnapshot(const Profile& p, int polyphonyOverride = -1);

}  // namespace gmb
