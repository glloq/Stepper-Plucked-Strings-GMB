// Persistent profile storage on the ESP32 (LittleFS + ArduinoJson).
//
// Serialises the pure-core Profile to/from the JSON schema documented in
// docs/ and the example instrument-profiles/. Stores at least 8 profiles plus
// the id of the start-up profile (spec §20). The Wi-Fi password is
// never written to an ordinary export.
#pragma once

#include <ArduinoJson.h>

#include <string>
#include <vector>

#include "../../core/configuration/Profile.h"

namespace gmb {

class ProfileStorage {
public:
    static constexpr int kMaxProfiles = 8;

    bool begin();  // mount LittleFS (see degraded())

    // True when the filesystem could not be mounted AND it had been initialised
    // before (an NVS marker proves it): rather than auto-formatting and wiping
    // every profile, storage stays read-unavailable until an explicit format().
    bool degraded() const { return degraded_; }
    // Explicit administrative format: reformats LittleFS and clears the degraded
    // state. Destroys all stored profiles — only called on a deliberate request.
    bool format();

    // Serialise / parse a single profile <-> JSON (no secrets by default).
    // fromJson accepts a JsonDocument, JsonObjectConst or the JsonVariant handed
    // to AsyncCallbackJsonWebHandler (avoids the ArduinoJson 7 ambiguity).
    static void toJson(const Profile& p, JsonDocument& doc);
    static bool fromJson(JsonVariantConst doc, Profile& out);

    std::string exportJson(const Profile& p, bool includeSecrets = false) const;
    bool importJson(const std::string& json, Profile& out) const;

    // Slot management.
    std::vector<std::string> list() const;
    bool load(int slot, Profile& out) const;
    bool save(int slot, const Profile& p);
    bool remove(int slot);

    int startupSlot() const;
    void setStartupSlot(int slot);

private:
    static std::string slotPath(int slot);
    bool degraded_ = false;
};

}  // namespace gmb
