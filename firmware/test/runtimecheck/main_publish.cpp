// Runtime check for the PUBLISH TRANSACTION — persisting a profile and accepting
// its activation must both happen, or neither.
//
// The two halves reached that guarantee at different times, and the seam between
// them was the last hole:
//
//     reserve  OK   a queue slot is claimed
//     prepare  OK   the new configuration is staged in /active.json.tmp
//     commit   FAIL the rename did not happen
//                   -> the machine must NOT be running the new profile
//
// The old order enqueued before committing, so at that point loop() could already
// have picked the activation up: the machine ran B with A on flash, and came back
// as A after a reboot. The response said so honestly, which is not the same as it
// not happening.
//
// What runs here is the REAL WebApi::publishProfile (compiled off-Arduino,
// which is why it lives outside that guard), the REAL ActivationCoordinator and the
// REAL CommandDispatcher, over a queue stub that actually fills up and a
// ProfileStorage whose prepare and commit can each be made to fail. The rest of
// WebApi — the routes, ArduinoJson — is not involved: the ordering is the claim,
// and the ordering is all in that one function.
//
// What it cannot prove: that LittleFS's rename is itself atomic on a real chip
// under a brownout. That is the assumption the whole model rests on, and it stays
// bench work.
#include <cstdio>
#include <string>
#include <vector>

#include "../../src/core/configuration/DeviceInstrument.h"
#include "../../src/core/configuration/Profile.h"
#include "../../src/platform/esp32/ActivationCoordinator.h"
#include "../../src/platform/esp32/ProfileStorage.h"
#include "../../src/platform/esp32/WebApi.h"

using namespace gmb;

// ---- the injectable storage --------------------------------------------------
//
// The real ProfileStorage.cpp is NOT compiled into this harness; these three
// definitions stand in for its non-Arduino stubs, which always fail and so could
// never show a successful commit. Everything else — the class, its declaration,
// the caller — is the real thing.
namespace {
bool g_prepareFails = false;
bool g_commitFails = false;
int g_prepareCount = 0;
int g_commitCount = 0;
int g_discardCount = 0;
bool g_tmpStaged = false;      // is there an uncommitted temp file right now?
std::string g_committed;       // the instrument name currently "on flash"
std::string g_staged;          // the instrument name staged in the temp file
}  // namespace

namespace gmb {
bool ProfileStorage::prepareActive(const Profile& p) {
    ++g_prepareCount;
    if (g_prepareFails) return false;
    g_staged = p.instrument.name;
    g_tmpStaged = true;
    return true;
}
bool ProfileStorage::commitActive() {
    ++g_commitCount;
    if (g_commitFails) return false;
    if (!g_tmpStaged) return false;   // nothing staged: a rename of nothing
    g_committed = g_staged;
    g_tmpStaged = false;
    return true;
}
void ProfileStorage::discardActive() {
    ++g_discardCount;
    g_tmpStaged = false;
    g_staged.clear();
}
}  // namespace gmb

// ---- harness ----------------------------------------------------------------
static int g_fail = 0;
static const char* g_case = "";
static void beginCase(const char* n) { g_case = n; }
static void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("  [FAIL] %s — %s\n", g_case, what);
        ++g_fail;
    }
}

// A profile that really passes ProfileValidator::isActivatable: one axis with its
// STEP/DIR/HOME pins and the shared ENABLE, no servos on a PCA (so no /OE needed).
static Profile activatableProfile(const char* name) {
    Profile p = Profile::makeDefault(name, 1, {60}, 12);
    p.instrument.name = name;
    auto& a = p.strings[0];
    a.enabled = true;
    a.transmission = Transmission::Custom;
    a.customStepsPerMm = 100.0;
    a.minPositionMm = 0.0;
    a.maxPositionMm = 200.0;
    a.maxSpeedMmS = 200.0;
    a.maxAccelMmS2 = 1000.0;
    p.pins.clear();
    auto pin = [&](const char* sig, SignalKind k, int8_t gpio) {
        PinAssignment pa;
        pa.signal = sig;
        pa.kind = k;
        pa.gpio = gpio;
        p.pins.push_back(pa);
    };
    pin("STEP1", SignalKind::Step, 4);
    pin("DIR1", SignalKind::Dir, 5);
    pin("HOME1", SignalKind::Home, 6);
    pin("ENABLE", SignalKind::Enable, 7);
    p.servos.clear();
    ServoConfig f;
    f.enabled = true;
    f.function = "finger";
    f.stringIndex = 0;
    f.source = ServoSource::DirectGpio;
    f.gpio = 10;
    p.servos.push_back(f);
    ServoConfig k;
    k.enabled = true;
    k.function = "pluck";
    k.stringIndex = 0;
    k.source = ServoSource::DirectGpio;
    k.gpio = 11;
    p.servos.push_back(k);
    return p;
}

// The rig: a dispatcher, the coordinator over it, and a WebContext wired exactly as
// main.cpp wires it.
struct Rig {
    CommandDispatcher dispatcher;
    ActivationCoordinator coord;
    ProfileStorage storage;
    WebApi api;
    Profile running;

    explicit Rig(int queueLen = 4) {
        running = activatableProfile("Running");
        dispatcher.begin(queueLen);
        coord.begin(&dispatcher, [this](const Profile& in, bool keepDeviceConfig) {
            if (!keepDeviceConfig) return in;
            return mergeProfile(deviceConfigOf(running), instrumentProfileOf(in), in);
        });
        WebContext ctx;
        ctx.storage = &storage;
        ctx.profile = &running;
        ctx.onReserveActivation = [this](const Profile& p, bool keep, Profile& out) {
            return coord.reserve(p, keep, out);
        };
        ctx.onPublishActivation = [this](uint32_t t) { return coord.publish(t); };
        ctx.onCancelActivation = [this](uint32_t t) { coord.cancel(t); };
        api.begin(ctx, 0);
    }

    WebApi::PublishResult publish(const Profile& p, bool keepDeviceConfig = false) {
        return api.publishProfile(p, keepDeviceConfig);
    }

    // What loop() would actually receive.
    std::vector<Profile> drain() {
        std::vector<Profile> out;
        dispatcher.drain(64, [&](const AppCommand& c) {
            if (c.type == CmdType::ActivateProfile && c.profile) out.push_back(*c.profile);
            return CmdOutcome::Succeeded;
        });
        return out;
    }
    std::vector<std::string> drainNames() {
        std::vector<std::string> names;
        for (const auto& p : drain()) names.push_back(p.instrument.name);
        return names;
    }
};

static void resetStorage() {
    g_prepareFails = g_commitFails = false;
    g_prepareCount = g_commitCount = g_discardCount = 0;
    g_tmpStaged = false;
    g_committed.clear();
    g_staged.clear();
}

int main() {
    std::printf("Publish transaction runtime check\n");

    // ---- the happy path: persisted AND accepted ------------------------------
    {
        beginCase("a good publish persists and activates");
        resetStorage();
        Rig rig;
        auto r = rig.publish(activatableProfile("Alpha"));
        check(r.accepted, "the activation is accepted");
        check(r.persisted, "the configuration is persisted");
        check(r.commandId != 0, "a command id is reported");
        check(r.httpStatus == 202, "202 Accepted");
        check(g_committed == "Alpha", "flash holds Alpha");
        auto seen = rig.drainNames();
        check(seen.size() == 1 && seen[0] == "Alpha", "loop() receives exactly Alpha");
    }

    // ---- THE defect: a failed commit must not leave an activation running -----
    {
        beginCase("a failed commit activates nothing");
        resetStorage();
        Rig rig;
        rig.publish(activatableProfile("Alpha"));      // establish a known flash state
        rig.drainNames();
        g_commitFails = true;
        auto r = rig.publish(activatableProfile("Bravo"));
        check(!r.accepted, "the activation is REFUSED");
        check(!r.persisted, "nothing was persisted");
        check(r.httpStatus == 507, "507 Insufficient Storage");
        check(g_committed == "Alpha", "flash still holds Alpha");
        check(g_discardCount == 1, "the staged temp file was discarded");
        // The whole point: loop() must never see Bravo. Before the reservation
        // existed it did, and the machine ran Bravo while flash held Alpha.
        auto seen = rig.drainNames();
        check(seen.empty(), "loop() sees NOTHING — the machine keeps running Alpha");
    }

    // ---- a failed prepare is the same story, one step earlier ----------------
    {
        beginCase("a failed prepare activates nothing");
        resetStorage();
        Rig rig;
        g_prepareFails = true;
        auto r = rig.publish(activatableProfile("Bravo"));
        check(!r.accepted, "the activation is refused");
        check(!r.persisted, "nothing was persisted");
        check(r.httpStatus == 507, "507 Insufficient Storage");
        check(g_commitCount == 0, "commit was never attempted");
        check(rig.drainNames().empty(), "loop() sees nothing");
    }

    // ---- a full queue must not change what boots next ------------------------
    {
        beginCase("a full command queue leaves the stored profile alone");
        resetStorage();
        Rig rig(/*queueLen=*/1);
        rig.publish(activatableProfile("Alpha"));   // fills the single slot
        check(g_committed == "Alpha", "Alpha is on flash");
        auto r = rig.publish(activatableProfile("Bravo"));
        check(!r.accepted, "the second publish is refused");
        check(!r.persisted, "and persists nothing");
        check(g_committed == "Alpha", "flash still holds Alpha, not Bravo");
        // Refused BEFORE the write is attempted: the reservation is what fails, so
        // a full queue never even stages a temp file.
        check(g_prepareCount == 1, "no second prepare was attempted");
        auto seen = rig.drainNames();
        check(seen.size() == 1 && seen[0] == "Alpha", "only Alpha reaches loop()");
    }

    // ---- an invalid profile is refused before anything is claimed ------------
    {
        beginCase("an invalid profile claims nothing");
        resetStorage();
        Rig rig;
        Profile bad = activatableProfile("Bad");
        bad.pins.clear();                       // no STEP/DIR/HOME: cannot be armed
        auto r = rig.publish(bad);
        check(!r.accepted, "refused");
        check(g_prepareCount == 0, "storage was never touched");
        check(rig.dispatcher.reserved() == 0, "no queue slot is held");
        check(!rig.coord.busy(), "no transaction is left in flight");
    }

    // ---- concurrency: two overlapping publishes -------------------------------
    //
    // The single /active.json.tmp is the reason. If B could stage while A was
    // between prepare and commit, B's bytes would overwrite A's and A's commit
    // would rename B's file into place: activating A while storing B. The
    // reservation makes the second one lose cleanly instead.
    {
        beginCase("a second publish is refused while one is in flight");
        resetStorage();
        Rig rig;
        Profile merged;
        uint32_t tokenA = rig.coord.reserve(activatableProfile("Alpha"), false, merged);
        check(tokenA != 0, "A reserves the transaction");
        Profile mergedB;
        uint32_t tokenB = rig.coord.reserve(activatableProfile("Bravo"), false, mergedB);
        check(tokenB == 0, "B is refused while A holds it");
        // A finishes; the slot is free again and B can go.
        check(rig.coord.publish(tokenA), "A publishes");
        check(!rig.coord.busy(), "the transaction is released");
        uint32_t tokenB2 = rig.coord.reserve(activatableProfile("Bravo"), false, mergedB);
        check(tokenB2 != 0, "B succeeds once A is done");
        rig.coord.cancel(tokenB2);
        auto seen = rig.drainNames();
        check(seen.size() == 1 && seen[0] == "Alpha", "only A ran");
    }

    // A cancelled transaction must give its queue slot back, or a device that ever
    // fails a write slowly loses its whole command capacity.
    {
        beginCase("a cancelled reservation returns its queue slot");
        resetStorage();
        Rig rig(/*queueLen=*/1);
        g_prepareFails = true;
        for (int i = 0; i < 5; ++i) rig.publish(activatableProfile("Doomed"));
        check(rig.dispatcher.reserved() == 0, "no reservation leaked");
        check(!rig.coord.busy(), "no transaction leaked");
        g_prepareFails = false;
        auto r = rig.publish(activatableProfile("Alpha"));
        check(r.accepted, "the queue still has its capacity after 5 failures");
    }

    // ---- the load path merges ONCE -------------------------------------------
    //
    // keepDeviceConfig=true keeps this machine's device half. What is stored has to
    // be what runs; the load route used to compute its own merge for storage, so
    // "what runs" and "what is stored" were two computations that had to agree.
    {
        beginCase("loading an instrument stores exactly what will run");
        resetStorage();
        Rig rig;
        rig.running.instrument.name = "Machine";
        rig.running.network.hostname = "bench-01";
        Profile slot = activatableProfile("Mandolin");
        slot.network.hostname = "someone-elses-box";   // device half from another rig
        auto r = rig.publish(slot, /*keepDeviceConfig=*/true);
        check(r.accepted && r.persisted, "accepted and persisted");
        check(g_committed == "Mandolin", "the stored instrument is the slot's");
        auto ran = rig.drain();
        check(ran.size() == 1 && ran[0].instrument.name == "Mandolin", "and it is what runs");
        check(ran.size() == 1 && ran[0].network.hostname == "bench-01",
              "this machine's hostname survived the load");
    }

    if (g_fail == 0) std::printf("\npublishcheck OK\n");
    else std::printf("\npublishcheck: %d failure(s)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
