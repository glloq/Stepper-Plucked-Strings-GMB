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

    // Upgrade a raw profile JSON in place from its stored `profileVersion` up to
    // kCurrentProfileVersion, applying one explicit migrateV{N-1}ToV{N} step at a
    // time (audit P1.12). Every load/import path runs this before fromJson() so old
    // configs keep working WITHOUT version special-cases piling up inside fromJson().
    // A profile already at (or above) the current version is left untouched.
    static void migrate(JsonDocument& doc);

    // On-disk slot storage format (audit P1.13). The DEVICE half (board / pins /
    // hardware notes / network) and the INSTRUMENT half (info / midi / selection /
    // power / pluck / axes+homing / servos) are persisted under separate `device` and
    // `instrument` sections, so the physical-machine config and the portable-
    // instrument config are structurally split on disk. These are thin RE-PARENTING
    // wrappers around toJson/fromJson — the flat INTERCHANGE format the web API and
    // import/export keep using unchanged — so the field logic has a single source and
    // is never duplicated. A legacy flat slot (no `device` section) is read through
    // the interchange path and rewritten split on the next save (a lazy migration;
    // see docs/DEVICE_INSTRUMENT.md).
    static void toSlotJson(const Profile& p, JsonDocument& doc);
    static bool fromSlotJson(JsonVariantConst doc, Profile& out);

    // ---- What actually boots -------------------------------------------------
    //
    // The 8 slots are a LIBRARY. They are not the running configuration, and they
    // were the wrong thing to boot from: the startup slot carries a device half
    // (board, pins, E-stop wiring, fitted hardware) captured whenever that slot was
    // last written, so rewiring the machine and publishing the change worked for the
    // session and then silently reverted at the next power-up. That also contradicted
    // the hot path, where loading a slot deliberately KEEPS this machine's device
    // config. Two files close it:
    //
    //   /device.json    this MACHINE's own config. One per device, never carried by
    //                   an instrument, and it wins over whatever a slot claims.
    //   /current.json   the instrument that is actually running, i.e. what was last
    //                   published. Boot restores this, so "what runs" == "what boots".
    //
    // Both are written when a configuration is ACCEPTED for activation, so publishing
    // is a real save. Neither replaces the library: `POST /api/profiles` still writes
    // a named slot, and nothing here overwrites one behind the user's back.
    //
    // Legacy installs have neither file; boot then falls back to the startup slot as
    // before and the two files appear on the first publish (a lazy migration).
    bool saveDevice(const Profile& p);
    // Overlays ONLY the device fields onto `inout`, leaving its instrument half
    // untouched. False when no device file is stored.
    bool loadDevice(Profile& inout) const;
    bool hasDevice() const;

    bool saveCurrent(const Profile& p);
    bool loadCurrent(Profile& out) const;
    bool hasCurrent() const;

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
#if defined(ARDUINO)
    // Durable write: temp file -> read back and re-parse -> keep a .bak -> rename.
    // A failure at any step leaves the previous file intact. `verify` re-parses the
    // temp file; a write that cannot be read back is a failed write, because the
    // failure mode that matters here is a truncated file that still looks present.
    static bool writeJsonAtomic(const std::string& finalPath, const JsonDocument& doc,
                                bool (*verify)(JsonVariantConst));
#endif
    bool degraded_ = false;
};

}  // namespace gmb
