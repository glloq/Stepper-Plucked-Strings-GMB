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
    // were the wrong thing to boot from: a slot carries a device half (board, pins,
    // E-stop wiring, fitted hardware) captured whenever it was last written, so
    // rewiring the machine and publishing worked for the session and then silently
    // reverted at the next power-up.
    //
    // ONE file holds what runs, replaced atomically. It carries both halves under the same split
    // `device` / `instrument` sections the slots use.
    //
    // This was two files, /device.json and /current.json, each individually atomic.
    // The PAIR was not: device could be written and current fail, leaving the next
    // boot to reassemble a new machine config with an old instrument — a
    // combination that never existed and was never validated. One file has one
    // commit point, so that state is unrepresentable.
    static constexpr const char* kActivePath = "/active.json";

    // Two-phase, because persisting and accepting an activation must succeed or
    // fail TOGETHER. Writing first and enqueuing after means a full command queue
    // returns 503 while the flash already holds the new configuration — the request
    // failed and the next boot changed anyway. Writing after means a flash failure
    // leaves the machine running something it will not come back as.
    //
    //   prepareActive()  write + verify a temp file. Nothing visible has changed.
    //   commitActive()   one rename makes it the active snapshot.
    //   discardActive()  drop the temp; the stored snapshot is untouched.
    bool prepareActive(const Profile& p);
    bool commitActive();
    void discardActive();

    // Convenience for callers with nothing to coordinate (there are none on the
    // activation path — this exists for tests and one-shot migration).
    bool saveActive(const Profile& p) { return prepareActive(p) && commitActive(); }

    // How reading the active snapshot went. `Missing` and `Unreadable` must NOT be
    // treated alike: a missing file is a first boot or a pre-/active.json install,
    // where falling back to a legacy location is right; an unreadable one is
    // CORRUPTION, and quietly booting some other stored instrument on a machine
    // whose own config just failed to parse is how a carriage ends up driven by
    // someone else's pin map.
    enum class LoadResult : uint8_t { Ok, Missing, Unreadable };
    LoadResult loadActive(Profile& out) const;

    // Legacy readers, kept only so an install predating /active.json migrates on
    // its first publish. Both return Missing when the file is absent.
    LoadResult loadLegacyCurrent(Profile& out) const;
    LoadResult loadLegacyDevice(Profile& inout) const;

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
    // The same write split at its commit point, so a caller can put a non-filesystem
    // step (accepting an activation) between "it is on flash" and "it is the file".
    static bool prepareJsonAtomic(const std::string& finalPath, const JsonDocument& doc,
                                  bool (*verify)(JsonVariantConst));
    static bool commitJsonAtomic(const std::string& finalPath);
    static void discardJsonAtomic(const std::string& finalPath);
#endif
    bool degraded_ = false;
};

}  // namespace gmb
