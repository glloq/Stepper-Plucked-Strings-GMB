// Stepper-Plucked-Strings-GMB — ESP32-S3 firmware entry point.
//
// Wires the pure-core logic (src/core/) to the ESP32 platform adapters
// (src/platform/esp32/). The core is unit-tested on the host; this file is the
// hardware integration and runs only on device.
//
// Boot sequence (cahier des charges §21.1 / §13): power-on safe → validate
// profile → home every axis (non-blocking) → only then arm for play.
#if defined(ARDUINO)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cmath>
#include <vector>

#include "core/configuration/Profile.h"
#include "core/configuration/ProfileValidator.h"
#include "core/gmb/GmbSysExService.h"
#include "core/instrument/InstrumentController.h"
#include "core/midi/MidiEvent.h"
#include "core/motion/HomingController.h"
#include "core/safety/SafetyManager.h"
#include "platform/esp32/MidiWifi.h"
#include "platform/esp32/Net.h"
#include "platform/esp32/ProfileStorage.h"
#include "platform/esp32/ServoBank.h"
#include "platform/esp32/StepperBank.h"
#include "platform/esp32/WebApi.h"

using namespace gmb;

namespace {

Profile g_profile;
ProfileStorage g_storage;
SafetyManager g_safety;
InstrumentController g_instrument;
GmbSysExService g_sysex;
StepperBank g_steppers;
ServoBank g_servos;
Net g_net;
MidiWifi g_midi;
WebApi g_web;

enum class AppPhase { Boot, Homing, Ready };
AppPhase g_phase = AppPhase::Boot;
bool g_degraded = false;  // Ready but with one or more axes disabled by a fault
uint32_t g_lastSharedStrumMs = 0;  // dedupe the shared strummer within a chord

// Pre-homing finger-lift: homing must not move a carriage while a finger is still
// pressed on the string. beginHoming() commands every finger to rest and waits
// until this deadline before the seek starts.
bool g_homingStarted = false;
uint32_t g_homeReleaseUntilMs = 0;

// Musical move watchdog: if a carriage does not reach its fret within this window
// the axis is faulted (covers a refused/stalled step-engine command).
constexpr uint32_t kMoveTimeoutMs = 4000;

// Two-phase profile activation: the OLD profile's fingers are driven to rest and
// allowed to lift BEFORE the old servo config is destroyed, so a profile that
// removes/reassigns a finger servo can't leave a finger pressed while the new
// profile homes. Non-null while an activation is waiting for the old fingers.
Profile* g_pendingProfile = nullptr;
uint32_t g_pendingActivateAtMs = 0;

std::vector<HomingController> g_homing;
std::vector<bool> g_anchored;
std::vector<bool> g_axisFaulted;  // runtime fault (homing fail, LIMIT, etc.)

int8_t g_estopPin = -1;
Debouncer g_estopDeb;  // debounced E-stop input (avoids a spurious trip)

// Pending test-note Note Offs (scheduled by /api/test/note). A small queue so a
// second test before the first ends does not drop the first note's release.
struct TestNoteOff { uint8_t channel; uint8_t note; uint32_t atMs; };
std::vector<TestNoteOff> g_testOffs;

// Per-string non-blocking playback scheduler.
struct StringSched {
    enum Phase { Idle, WaitStopped, ReleasingFinger, MovingToFret, PressingFinger,
                 Settling, Ready }
        phase = Idle;
    uint32_t phaseStartMs = 0;
    uint32_t commandId = 0;
    int fingerIndex = -1;
    uint32_t readySinceMs = 0;  // when this string reached Ready (shared-strum sync)
    uint32_t dampUntilMs = 0;   // don't move until the damper has acted (replace)
};
std::vector<StringSched> g_sched;

// ---- Web -> loop() command queue (P0: no mechanical state off the main loop) --
//
// ESPAsyncWebServer runs its callbacks in the AsyncTCP task, which can execute
// in parallel with loop(). If those callbacks touched g_profile / g_instrument /
// g_steppers / g_servos directly they could reallocate a std::vector while
// loop() is iterating it. Instead every mutating request only ENQUEUES a command
// here; loop() is the SOLE owner of the mechanical state and drains the queue
// sequentially. Read-only handlers take g_stateMutex so a reallocation in loop()
// (profile reload, capability rebuild) can never be seen half-done.
enum class CmdType : uint8_t { Panic, Reset, ActivateProfile, TestNote, TestServo };
struct AppCommand {
    CmdType type;
    uint32_t id = 0;             // for result tracking (GET /api/commands)
    Profile* profile = nullptr;  // owned by the command (ActivateProfile)
    uint8_t channel = 0, note = 0, velocity = 0;
    uint16_t durationMs = 0;
    int16_t servoIndex = -1;
    bool servoActive = false;
};

// Result registry so a 202-accepted command can be followed up by the web UI:
// GET /api/commands?id=N reports queued / succeeded / refused (audit P1-18).
std::atomic<uint32_t> g_nextCmdId{1};
struct CmdResult { uint32_t id = 0; uint8_t state = 0; };  // 0=queued 1=done 2=refused
constexpr int kCmdResultRing = 16;
CmdResult g_cmdResults[kCmdResultRing];
SemaphoreHandle_t g_resultMutex = nullptr;  // guards the tiny result ring

void setCommandResult(uint32_t id, uint8_t state) {
    if (id == 0) return;
    if (g_resultMutex) xSemaphoreTake(g_resultMutex, portMAX_DELAY);
    for (auto& r : g_cmdResults)  // update in place if already present
        if (r.id == id) { r.state = state; if (g_resultMutex) xSemaphoreGive(g_resultMutex); return; }
    // else overwrite the oldest slot (ring)
    static int next = 0;
    g_cmdResults[next] = {id, state};
    next = (next + 1) % kCmdResultRing;
    if (g_resultMutex) xSemaphoreGive(g_resultMutex);
}

// "queued" / "succeeded" / "refused" / "unknown" for a command id.
std::string commandStateStr(uint32_t id) {
    const char* s = "unknown";
    if (g_resultMutex) xSemaphoreTake(g_resultMutex, portMAX_DELAY);
    for (const auto& r : g_cmdResults)
        if (r.id == id) { s = r.state == 0 ? "queued" : r.state == 1 ? "succeeded" : "refused"; break; }
    if (g_resultMutex) xSemaphoreGive(g_resultMutex);
    return s;
}
QueueHandle_t g_cmdQueue = nullptr;      // holds AppCommand* pointers
// In-memory runtime state (profile / sysex snapshot / status). loop() takes this
// only for short in-memory work — NEVER across a LittleFS write — so the safety
// loop can never stall on flash.
SemaphoreHandle_t g_stateMutex = nullptr;
// LittleFS access (save/load/delete/read/list). Held only by the AsyncTCP web
// task; loop() NEVER takes it, so a long flash write can never block the safety
// loop (audit P0-1).
SemaphoreHandle_t g_storageMutex = nullptr;

// A STOP must never be lost or delayed behind other queued commands: /api/panic
// sets this flag (lock-free, no allocation) and loop() honours it FIRST, before
// draining anything else. Separate from the FreeRTOS queue on purpose.
std::atomic<bool> g_panicRequested{false};

// Enqueue a command (called from the AsyncTCP task). Returns false if the queue
// is full so the caller can report back-pressure instead of silently dropping.
// Returns the assigned command id (0 if the queue is full / unavailable).
uint32_t enqueueCommand(const AppCommand& in) {
    if (!g_cmdQueue) return 0;
    AppCommand c = in;
    c.id = g_nextCmdId.fetch_add(1);
    AppCommand* h = new AppCommand(c);
    if (xQueueSend(g_cmdQueue, &h, 0) != pdTRUE) {
        delete h->profile;  // transfer failed: don't leak the owned profile
        delete h;
        return 0;
    }
    setCommandResult(c.id, 0);  // queued
    return c.id;
}

// RAII guard for the shared-state mutex (used by loop() around reloads and by the
// read-only web handlers around their reads).
struct StateGuard {
    StateGuard() { if (g_stateMutex) xSemaphoreTake(g_stateMutex, portMAX_DELAY); }
    ~StateGuard() { if (g_stateMutex) xSemaphoreGive(g_stateMutex); }
};

int8_t pinOf(const char* signal) {
    for (const auto& a : g_profile.pins)
        if (a.signal == signal) return a.gpio;
    return -1;
}

void buildStepperPins(std::vector<AxisPins>& out, int8_t& enablePin) {
    enablePin = -1;
    out.assign(g_profile.strings.size(), AxisPins{});
    for (const auto& a : g_profile.pins) {
        if (a.signal == "ENABLE") enablePin = a.gpio;
        for (size_t i = 0; i < out.size(); ++i) {
            const std::string idx = std::to_string(i + 1);
            if (a.signal == "STEP" + idx) out[i].step = a.gpio;
            else if (a.signal == "DIR" + idx) out[i].dir = a.gpio;
            else if (a.signal == "HOME" + idx) out[i].home = a.gpio;
            else if (a.signal == "LIMIT" + idx) out[i].limit = a.gpio;
        }
    }
}

// Reallocates the per-string vectors. The CALLER must hold g_stateMutex around
// this (and around the g_profile assignment that precedes an activation) so a web
// read never observes the runtime half-rebuilt. g_stateMutex is only ever held
// for in-memory work — never across a LittleFS write — so loop() cannot stall on
// flash (see g_storageMutex).
void applyProfile() {
    g_instrument.load(g_profile);
    g_sysex.rebuild(g_profile);
    g_sched.assign(g_profile.strings.size(), StringSched{});

    std::vector<AxisPins> axisPins;
    int8_t enablePin = -1;
    buildStepperPins(axisPins, enablePin);
    std::vector<bool> homeActiveHigh, limitActiveHigh;
    for (size_t i = 0; i < g_profile.strings.size(); ++i) {
        bool hah = i < g_profile.homing.size() ? g_profile.homing[i].sensorActiveHigh : false;
        bool lah = i < g_profile.homing.size() ? g_profile.homing[i].limitActiveHigh : false;
        homeActiveHigh.push_back(hah);
        limitActiveHigh.push_back(lah);
    }
    g_steppers.begin(g_profile.strings, axisPins, enablePin, homeActiveHigh,
                     limitActiveHigh);
    g_servos.begin(g_profile.servos, pinOf("SDA"), pinOf("SCL"), pinOf("SERVO_OE"));

    g_homing.assign(g_profile.strings.size(), HomingController{});
    g_anchored.assign(g_profile.strings.size(), false);
    g_axisFaulted.assign(g_profile.strings.size(), false);
    for (size_t i = 0; i < g_profile.strings.size(); ++i) {
        if (i < g_profile.homing.size()) g_homing[i].configure(g_profile.homing[i]);
    }

    // The E-stop pin belongs to the (possibly new) profile — re-resolve it here
    // so a profile change updates which pin is monitored.
    g_estopPin = pinOf("ESTOP");
    if (g_estopPin >= 0) pinMode(g_estopPin, INPUT_PULLUP);
    g_estopDeb.configure(3, true);  // idle = HIGH (not pressed, active-low input)
}

// Rebuild the capability snapshot from a runtime copy of the profile that
// excludes every disabled or runtime-faulted axis, and update g_degraded.
// Returns the number of playable axes.
int rebuildRuntimeCapabilities() {
    StateGuard lock;  // g_sysex.rebuild reallocates the snapshot read by web GETs
    Profile rp = g_profile;
    int originallyEnabled = 0, ready = 0;
    for (size_t i = 0; i < rp.strings.size(); ++i) {
        if (g_profile.strings[i].enabled) ++originallyEnabled;
        if (!rp.strings[i].enabled || (i < g_axisFaulted.size() && g_axisFaulted[i]))
            rp.strings[i].enabled = false;
        else
            ++ready;
    }
    g_degraded = ready < originallyEnabled;
    g_profile.capabilitiesRevision++;
    rp.capabilitiesRevision = g_profile.capabilitiesRevision;
    g_sysex.rebuild(rp);
    return ready;
}

// Push a spontaneous "capabilities changed" SysEx (block 8) to the last host that
// queried us, so General-MIDI-Boop learns of a runtime change without polling.
void notifyCapabilitiesChanged() {
    if (!g_midi.hasLastSender()) return;
    std::vector<uint8_t> msg = g_sysex.notification(0x01);  // bit0 = caps changed
    if (!msg.empty()) g_midi.notifyLastSender(msg.data(), msg.size());
}

void neutraliseAll();  // defined below; needed by faultRuntimeAxis

// Central runtime axis-fault path (LIMIT, motor/servo error, homing failure).
void faultRuntimeAxis(size_t i, const char* reason, uint32_t nowMs) {
    if (i >= g_instrument.stringCount()) return;
    g_steppers.emergencyStop(i);
    g_instrument.faultString(i);
    if (i < g_sched.size()) g_sched[i] = StringSched{};
    if (i < g_anchored.size()) g_anchored[i] = false;
    if (i < g_axisFaulted.size()) g_axisFaulted[i] = true;
    g_safety.recordFault("axis", std::string(reason) + " on axis " + std::to_string(i),
                         nowMs);
    int working = rebuildRuntimeCapabilities();
    notifyCapabilitiesChanged();  // push block-8 so GMB learns of the change
    // If the last operational axis just failed, the instrument can no longer play
    // anything: neutralise and latch a panic rather than sitting "armed" with zero
    // strings (which would also emit a bogus 0..0 capability range).
    if (working <= 0) {
        neutraliseAll();
        g_safety.panic("no operational axes remain", nowMs);
    }
}

bool safetyLocked() {
    SafetyState s = g_safety.state();
    return s == SafetyState::Panic || s == SafetyState::EmergencyStop;
}

// Start homing only when it is safe to move. Refuses if a panic/E-stop is
// latched, if the profile is invalid, or if a required motor could not attach a
// hardware step generator (cahier des charges §13/§21).
bool beginHoming(uint32_t nowMs) {
    if (safetyLocked()) return false;
    // Never enable drivers while a hardware E-stop is physically asserted, even
    // if the software state has not caught up yet (closes the power-on window).
    if (g_estopPin >= 0 && digitalRead(g_estopPin) == LOW) {
        g_safety.emergencyStop(nowMs);
        return false;
    }
    if (!ProfileValidator::isActivatable(g_profile)) return false;
    if (g_steppers.attachFault() || g_servos.directAttachFault() ||
        g_servos.pcaAttachFault()) {
        g_safety.recordFault("attach",
                             "a motor/servo/PCA9685 could not attach or respond", nowMs);
        return false;
    }
    // Reconfiguration/homing must start from PowerOnSafe, never from Armed: a
    // stale Armed state would let /api/test/servo drive a servo mid-homing and
    // would make the final arm() a silent no-op. safetyLocked() (Panic/E-stop)
    // is already refused above, so this only demotes a lingering Armed state.
    g_safety.reset();  // -> PowerOnSafe; doHoming re-arms once axes are homed
    g_degraded = false;
    g_steppers.enableDrivers(true);
    g_servos.outputEnable(true);   // servos hold their rest position (fingers up)

    // Drive every finger to its REST position and wait for it to lift before any
    // carriage moves — a still-pressed finger must never drag along the string as
    // the axis seeks HOME. The homing controllers are started only once this
    // deadline passes (see doHoming).
    uint32_t releaseWaitMs = 0;
    for (size_t i = 0; i < g_homing.size(); ++i) {
        g_anchored[i] = false;
        if (!g_profile.strings[i].enabled) {
            g_instrument.string(i).disable();  // never home a disabled axis
            continue;
        }
        g_instrument.string(i).setHoming();
        int fi = g_servos.fingerIndex(static_cast<int>(i));
        if (fi >= 0) {
            g_servos.release(fi);  // command finger up (to rest)
            uint32_t w = static_cast<uint32_t>(g_servos.travelMs(fi)) +
                         g_servos.settleMs(fi);
            if (w > releaseWaitMs) releaseWaitMs = w;
        }
    }
    g_homingStarted = false;
    g_homeReleaseUntilMs = nowMs + releaseWaitMs;
    g_phase = AppPhase::Homing;
    return true;
}

// Whether any endstop is currently asserted (blocks a reset / re-home).
bool anyLimitActive() {
    for (size_t i = 0; i < g_steppers.count(); ++i)
        if (g_steppers.limitActive(i)) return true;
    return false;
}

// Explicit recovery after a panic / E-stop: only proceeds when the E-stop is
// released, no LIMIT is asserted, the profile is valid and all channels attached.
bool doReset(uint32_t nowMs) {
    if (g_estopPin >= 0 && digitalRead(g_estopPin) == LOW) return false;  // still pressed
    if (anyLimitActive()) return false;
    if (!ProfileValidator::isActivatable(g_profile)) return false;
    if (g_steppers.attachFault() || g_servos.directAttachFault() ||
        g_servos.pcaAttachFault())
        return false;
    g_safety.reset();                 // Panic/EStop -> PowerOnSafe
    g_safety.clearFaults();
    return beginHoming(nowMs);         // mandatory re-home before playing again
}

void doHoming(uint32_t nowMs) {
    // Hold every axis still until the fingers have physically lifted, then start
    // the homing controllers (their internal timers begin here, not before).
    if (!g_homingStarted) {
        if (static_cast<int32_t>(nowMs - g_homeReleaseUntilMs) < 0) return;
        for (size_t i = 0; i < g_homing.size(); ++i)
            if (g_profile.strings[i].enabled) g_homing[i].start(nowMs);
        g_homingStarted = true;
    }

    bool allDone = true;
    int active = 0, faulted = 0;
    for (size_t i = 0; i < g_homing.size(); ++i) {
        if (!g_profile.strings[i].enabled) continue;  // disabled axes are skipped
        ++active;
        // Watch the LIMIT switch during EVERY homing phase: hitting the opposite
        // endstop means the HOME sensor was missed — hard-stop and fault the axis
        // instead of grinding on until the timeout / max distance.
        if (!g_homing[i].ready() && !g_homing[i].failed() &&
            g_steppers.limitActive(i)) {
            g_steppers.emergencyStop(i);
            g_homing[i].abort(HomingFault::LimitTriggered);
        }
        if (g_homing[i].failed()) {
            g_instrument.faultString(i);  // remove from allocator + selection too
            g_axisFaulted[i] = true;
            g_steppers.emergencyStop(i);  // HARD stop: a failed axis must not keep
                                          // decelerating past the arming instant
            ++faulted;
            continue;
        }
        if (g_homing[i].ready()) {
            if (!g_anchored[i]) {
                // Anchor the coordinate system so the home sensor is 0 mm.
                g_steppers.setPositionReference(i, g_homing[i].restOffsetMm());
                g_instrument.string(i).homingDone();
                g_anchored[i] = true;
            }
            continue;
        }
        allDone = false;
        bool rawHigh = g_steppers.homeRawHigh(i);
        HomingCommand cmd = g_homing[i].update(nowMs, rawHigh, g_steppers.positionMm(i),
                                               g_steppers.isRunning(i));
        switch (cmd.kind) {
            case MoveKind::Stop: g_steppers.stop(i); break;
            case MoveKind::MoveVelocity: g_steppers.setVelocityMm(i, cmd.velocityMmS); break;
            case MoveKind::MoveTo: g_steppers.moveToMmRaw(i, cmd.targetMm); break;
        }
    }
    if (!allDone) return;

    // Never arm while a faulted axis is still physically moving (shared ENABLE
    // stays live for all drivers): wait for every faulted axis to report stopped.
    for (size_t i = 0; i < g_homing.size(); ++i) {
        if (g_axisFaulted[i] && g_steppers.isRunning(i)) return;  // still braking
    }

    // Refuse to arm with zero working axes: neutralise and stay safely in Boot.
    if (active - faulted <= 0) {
        g_steppers.enableDrivers(false);
        g_servos.neutraliseAll();
        g_safety.recordFault("homing", "no axis could be homed — not arming", nowMs);
        g_phase = AppPhase::Boot;
        return;
    }

    g_phase = AppPhase::Ready;
    g_safety.arm(true, true);  // profile already validated before homing
    if (faulted > 0) {
        // Announce only the axes that actually work (cahier des charges §13.2).
        rebuildRuntimeCapabilities();
        notifyCapabilitiesChanged();
        g_safety.recordFault("homing",
                             std::to_string(faulted) + " axis/axes failed homing "
                             "(degraded run)", nowMs);
    } else {
        g_degraded = false;
    }
}

void neutraliseAll() {
    g_instrument.panic();
    g_steppers.stopAll();
    g_steppers.enableDrivers(false);
    g_servos.neutraliseAll();
    for (auto& s : g_sched) s = StringSched{};
    // A panic / E-stop supersedes any deferred profile activation.
    delete g_pendingProfile;
    g_pendingProfile = nullptr;
    g_phase = AppPhase::Boot;
}

void doPanic() {
    neutraliseAll();
    // A hardware E-stop outranks a software panic: never downgrade a latched
    // EmergencyStop to Panic (audit P1-17). The machine is already neutralised.
    if (g_safety.state() != SafetyState::EmergencyStop)
        g_safety.panic("web/CC panic", millis());
}

// Hardware E-stop: latches the distinct EmergencyStop state (not Panic).
void doEmergencyStop() {
    neutraliseAll();
    g_safety.emergencyStop(millis());
}

// ---- loop-side command handlers (only ever called from drainCommands) --------

// Phase 1 of activation: validate, stop the motors, and drive the OLD profile's
// fingers to rest. The teardown + reconfigure + re-home is deferred until the old
// fingers have physically lifted (servicePendingActivation).
bool doActivateProfile(const Profile& p, uint32_t nowMs) {
    if (!ProfileValidator::isActivatable(p)) return false;
    // Refuse to (re)start motion while a panic / E-stop is latched; the user must
    // explicitly reset first (POST /api/reset).
    if (safetyLocked()) return false;
    // Full stop of the carriages and release of the current notes, but KEEP the
    // servo outputs powered so the old fingers can be driven up.
    g_instrument.panic();
    g_steppers.stopAll();
    g_steppers.enableDrivers(false);
    for (auto& s : g_sched) s = StringSched{};
    g_phase = AppPhase::Boot;

    // Drive every current servo to rest (fingers up) before the old config is
    // destroyed, and wait for the slowest to travel + settle.
    g_servos.outputEnable(true);
    uint32_t wait = 0;
    for (size_t i = 0; i < g_servos.count(); ++i) {
        g_servos.release(static_cast<int>(i));
        uint32_t w = static_cast<uint32_t>(g_servos.travelMs(static_cast<int>(i))) +
                     g_servos.settleMs(static_cast<int>(i));
        if (w > wait) wait = w;
    }
    delete g_pendingProfile;
    g_pendingProfile = new Profile(p);
    g_pendingActivateAtMs = nowMs + wait;
    return true;
}

// Phase 2: once the old fingers have lifted, tear down the old profile, swap in
// the new one and re-home. Runs from loop().
void servicePendingActivation(uint32_t nowMs) {
    if (!g_pendingProfile) return;
    if (static_cast<int32_t>(nowMs - g_pendingActivateAtMs) < 0) return;  // still lifting
    g_servos.neutraliseAll();  // now safe to cut the old servo outputs
    {
        StateGuard lock;  // atomic profile swap + runtime rebuild (P0-2)
        g_profile = *g_pendingProfile;
        g_profile.capabilitiesRevision++;
        applyProfile();  // re-resolves pins incl. ESTOP, reinitialises hardware
    }
    delete g_pendingProfile;
    g_pendingProfile = nullptr;
    beginHoming(nowMs);  // re-home after a mechanical change (§16)
}

// Web test note: only when Ready, and only if we can guarantee its Note Off.
bool doTestNote(uint8_t channel, uint8_t note, uint8_t vel, uint16_t durationMs,
                uint32_t nowMs) {
    if (g_phase != AppPhase::Ready) return false;
    // Never emit a Note On we cannot later release: refuse if the deferred
    // Note-Off queue is full (would otherwise leave the note stuck on).
    if (g_testOffs.size() >= 16) return false;
    MidiEvent on;
    on.type = static_cast<uint8_t>(MidiType::NoteOn);
    on.channel = channel; on.data1 = note; on.data2 = vel;
    on.timestampUs = micros();
    on.source = static_cast<uint8_t>(MidiSource::WebUiTest);
    g_instrument.handleEvent(on, on.timestampUs);
    uint32_t offAt = nowMs + (durationMs ? durationMs : 500u);
    g_testOffs.push_back({channel, note, offAt});
    return true;
}

// Web servo test: only when armed and only for a real, enabled servo. Uses
// press()/release() (not the raw toActive/toRest) so the servo's runtime mode is
// updated and update() honours disableAtRest correctly.
bool doTestServo(int index, bool active) {
    if (!g_safety.actuatorsAllowed()) return false;
    if (!g_servos.commandable(index)) return false;
    if (active) g_servos.press(index); else g_servos.release(index);
    return true;
}

// Discard every queued command without executing it (used after a panic so a
// stale profile activation / test can't fire once the STOP has latched).
void purgeCommands() {
    if (!g_cmdQueue) return;
    AppCommand* c = nullptr;
    while (xQueueReceive(g_cmdQueue, &c, 0) == pdTRUE) {
        delete c->profile;
        delete c;
    }
}

// Honour a pending STOP before anything else. Returns true if a panic ran, so
// the caller can skip the rest of this tick's command/motion work.
bool servicePanic(uint32_t nowMs) {
    (void)nowMs;
    if (!g_panicRequested.exchange(false)) return false;
    doPanic();
    purgeCommands();  // drop anything queued behind the STOP
    return true;
}

// Drain a BOUNDED number of queued web commands so a long burst (e.g. many
// profile activations) can never starve the E-stop / panic checks that run each
// loop. The rest wait for the next tick.
void drainCommands(uint32_t nowMs) {
    if (!g_cmdQueue) return;
    static constexpr int kMaxCommandsPerTick = 2;
    AppCommand* c = nullptr;
    for (int n = 0; n < kMaxCommandsPerTick && xQueueReceive(g_cmdQueue, &c, 0) == pdTRUE;
         ++n) {
        bool ok = true;
        switch (c->type) {
            case CmdType::Panic: doPanic(); purgeCommands(); break;
            case CmdType::Reset: ok = doReset(nowMs); break;
            case CmdType::ActivateProfile:
                ok = c->profile && doActivateProfile(*c->profile, nowMs);
                break;
            case CmdType::TestNote:
                ok = doTestNote(c->channel, c->note, c->velocity, c->durationMs, nowMs);
                break;
            case CmdType::TestServo:
                ok = doTestServo(c->servoIndex, c->servoActive);
                break;
        }
        setCommandResult(c->id, ok ? 1 : 2);  // succeeded / refused
        delete c->profile;  // owned copy (null for non-profile commands)
        delete c;
    }
}

// Does a given string rely on the shared strummer (shared modes, or no
// individual plectrum of its own)?
bool usesSharedStrum(size_t i) {
    PluckMode mode = g_profile.instrument.pluckMode;
    int pi = g_servos.pluckIndex(static_cast<int>(i));
    return (mode == PluckMode::SharedStrum || mode == PluckMode::Both) || pi < 0;
}

// Shared-strum barrier: every OTHER active string in the SAME chord (strum group)
// that the shared strummer will sweep must have reached its fret position before
// the sweep fires — one sweep per chord, and a prepared or unrelated note (a
// different group) never blocks it.
bool sharedGroupPositioned(uint32_t group) {
    if (group == 0) return true;
    for (size_t j = 0; j < g_instrument.stringCount(); ++j) {
        const StringTarget& t = g_instrument.target(j);
        if (!t.active || t.strumGroup != group) continue;
        if (!usesSharedStrum(j)) continue;
        if (g_sched[j].phase != StringSched::Ready) return false;  // still travelling
    }
    return true;
}

// Drive one string's mechanical sequence toward a plucked note.
void tickString(size_t i, uint32_t nowMs) {
    StringController& sc = g_instrument.string(i);
    const StringTarget& tgt = g_instrument.target(i);
    StringSched& sch = g_sched[i];

    if (!tgt.active) {
        if (sch.phase == StringSched::Idle) return;
        if (sch.phase != StringSched::WaitStopped) {
            // Note released / cancelled: STOP the carriage (a Note Off during a
            // move must not let it finish travelling), lift the finger, damp.
            g_steppers.stop(i);
            int fi = g_servos.fingerIndex(static_cast<int>(i));
            if (fi >= 0) g_servos.release(fi);
            int di = g_servos.damperIndex(static_cast<int>(i));
            if (di >= 0) g_servos.strike(di);
            sch.phase = StringSched::WaitStopped;
        }
        // Only declare the axis idle once the carriage has REALLY stopped, so a
        // note accepted right after can't issue a moveTo into a still-decelerating
        // motor (the musical analogue of the homing brake states).
        if (!g_steppers.isRunning(i)) {
            sc.dampingDone();
            sch.phase = StringSched::Idle;
            sch.commandId = 0;
        }
        return;
    }

    // A LIMIT switch tripped faults this axis and removes it from service
    // (allocator + selection + capabilities), without disturbing the others.
    if (g_steppers.limitActive(i)) {
        faultRuntimeAxis(i, "LIMIT tripped", nowMs);
        return;
    }
    // HOME is also a physical extremity. Asserting it while the carriage position
    // says it is well away from home is a position/reference fault (lost steps,
    // drift, stuck/inverted sensor) — fault rather than trust a bad coordinate.
    // Positions legitimately near home (open string / low frets) are not faulted.
    // Home is anchored to 0 mm, so the ABSOLUTE distance from 0 is the mismatch
    // (the axis coordinate can run negative depending on the homing direction).
    static constexpr double kHomeMismatchMm = 10.0;
    if (g_steppers.homeActive(i) && std::fabs(g_steppers.positionMm(i)) > kHomeMismatchMm) {
        faultRuntimeAxis(i, "HOME asserted away from home (position drift)", nowMs);
        return;
    }

    if (sch.commandId != tgt.commandId) {
        // New note replacing a previous one on this string (e.g. the allocator's
        // ReplaceOldest): explicitly damp the still-vibrating string AND wait for
        // the damper's travel/settle before the carriage moves, so a ringing
        // string is not dragged to a new fret and re-plucked (audit P1-7).
        sch.dampUntilMs = nowMs;
        if (sch.phase != StringSched::Idle && sch.phase != StringSched::WaitStopped) {
            int di = g_servos.damperIndex(static_cast<int>(i));
            if (di >= 0) {
                g_servos.strike(di);
                sch.dampUntilMs = nowMs + g_servos.travelMs(di) + g_servos.settleMs(di);
            }
        }
        // Lift the finger and WAIT for it to travel up before moving the carriage,
        // so the finger never drags along the string (§16).
        sch.commandId = tgt.commandId;
        sch.fingerIndex = g_servos.fingerIndex(static_cast<int>(i));
        if (sch.fingerIndex >= 0) g_servos.release(sch.fingerIndex);
        sch.phase = StringSched::ReleasingFinger;
        sch.phaseStartMs = nowMs;
    }

    switch (sch.phase) {
        case StringSched::ReleasingFinger:
            // Start moving only once the finger has lifted, the damper (if any) has
            // acted, AND the carriage has fully stopped (a new note arriving during
            // a cancel deceleration must not issue a moveTo into a moving motor).
            if ((sch.fingerIndex < 0 ||
                 nowMs - sch.phaseStartMs >= g_servos.travelMs(sch.fingerIndex)) &&
                static_cast<int32_t>(nowMs - sch.dampUntilMs) >= 0 &&
                g_steppers.atTarget(i)) {
                g_steppers.moveToMm(i, tgt.positionMm);
                sch.phase = StringSched::MovingToFret;
                sch.phaseStartMs = nowMs;
            }
            break;
        case StringSched::MovingToFret:
            // Arrived only when the carriage is stopped AND actually at the fret
            // position (a refused/interrupted move must not be read as "reached").
            if (g_steppers.reachedTarget(i)) {
                sc.motionReached();
                if (sc.openString() || sch.fingerIndex < 0) {
                    sch.phase = StringSched::Ready;  // no finger press for open string
                    sch.readySinceMs = nowMs;
                } else {
                    g_servos.press(sch.fingerIndex);
                    sch.phase = StringSched::PressingFinger;
                    sch.phaseStartMs = nowMs;
                }
            } else if (nowMs - sch.phaseStartMs >= kMoveTimeoutMs) {
                // The move never completed (command refused by the step engine, a
                // stall, or a stop far from target): fault the axis instead of
                // waiting forever in MovingToFret.
                faultRuntimeAxis(i, "move did not reach target (timeout)", nowMs);
            }
            break;
        case StringSched::PressingFinger:
            if (nowMs - sch.phaseStartMs >= g_servos.travelMs(sch.fingerIndex)) {
                sc.fingerPressed();
                sch.phase = StringSched::Settling;
                sch.phaseStartMs = nowMs;
            }
            break;
        case StringSched::Settling:
            if (nowMs - sch.phaseStartMs >= g_servos.settleMs(sch.fingerIndex)) {
                sc.settled();
                sch.phase = StringSched::Ready;
                sch.readySinceMs = nowMs;
            }
            break;
        case StringSched::Ready: {
            if (!sc.pluckArmed()) break;  // not armed (prepared / already plucked)
            PluckMode mode = g_profile.instrument.pluckMode;
            int pi = g_servos.pluckIndex(static_cast<int>(i));
            bool doIndividual = (mode == PluckMode::Individual ||
                                 mode == PluckMode::Both) && pi >= 0;
            bool doShared = usesSharedStrum(i);
            // Shared-strum barrier: wait until the whole active shared group is in
            // position (or a bounded timeout, so a stuck string can't deadlock the
            // chord) before firing — one synchronised sweep per chord.
            if (doShared) {
                static constexpr uint32_t kMaxStrumWaitMs = 250;
                if (!sharedGroupPositioned(tgt.strumGroup) &&
                    nowMs - sch.readySinceMs < kMaxStrumWaitMs) {
                    break;  // stay armed in Ready, keep waiting for the chord group
                }
            }
            if (!sc.executePluck(tgt.commandId)) break;
            if (doIndividual) g_servos.strike(pi, tgt.intensity);
            if (doShared) {
                int si = g_servos.sharedStrumIndex();
                // One sweep per chord: dedupe strikes within a short window.
                static constexpr uint32_t kStrumDedupeMs = 30;
                if (si >= 0 && nowMs - g_lastSharedStrumMs >= kStrumDedupeMs) {
                    g_servos.strike(si, tgt.intensity);
                    g_lastSharedStrumMs = nowMs;
                }
            }
            break;
        }
        case StringSched::WaitStopped:
        case StringSched::Idle:
            break;
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    g_safety.boot();  // drivers off, servos neutralised (cahier des charges §21.1)

    // Web -> loop() command channel + shared-state mutex, created before the web
    // server so the first request is already safe.
    g_cmdQueue = xQueueCreate(16, sizeof(AppCommand*));
    g_stateMutex = xSemaphoreCreateMutex();
    g_storageMutex = xSemaphoreCreateMutex();
    g_resultMutex = xSemaphoreCreateMutex();

    g_storage.begin();
    if (g_storage.degraded()) {
        // A previously-initialised filesystem that won't mount: don't auto-wipe.
        g_safety.recordFault("storage",
            "LittleFS unmountable — profiles unavailable; POST /api/storage/format "
            "to reformat", millis());
    }
    // Never configure GPIO (STEP/DIR/HOME/LIMIT/ENABLE/I²C/PCA/LEDC) from a
    // profile that fails semantic validation: fall back to the safe default so a
    // corrupt or malicious stored profile can't drive the pins at boot (§21.1).
    if (!g_storage.load(g_storage.startupSlot(), g_profile) ||
        !ProfileValidator::isActivatable(g_profile)) {
        g_profile = Profile::makeDefault("Ukulele", 4, {67, 60, 64, 69}, 12);
    }
    // Stable per-device SysEx identity from the ESP32 MAC, so two instruments on
    // the same network are distinguishable (set before applyProfile's rebuild).
    {
        uint64_t mac = ESP.getEfuseMac();
        uint8_t id[5];
        for (int i = 0; i < 5; ++i) id[i] = static_cast<uint8_t>((mac >> (8 * i)) & 0x7F);
        g_sysex.setDeviceId(id);
    }
    { StateGuard lock; applyProfile(); }  // resolves the E-stop pin from the profile
    // Answer StringConfig discovery with the richer v2 block (CC bounds, offsets,
    // per-string frets, string mapping/order). The block carries its own version
    // byte so a v1-only client can still detect and skip it.
    g_sysex.setUseV2(true);

    // Wi-Fi secrets live in NVS, never in the exportable profile (§20).
    Preferences prefs;
    prefs.begin("gmb", true);
    String staPass = prefs.getString("wifipass", "");
    String apPass = prefs.getString("appass", "");
    prefs.end();
    g_net.begin(g_profile.network, staPass.c_str(), apPass.c_str());
    g_midi.begin(5006);

    WebContext ctx;
    ctx.profile = &g_profile;
    ctx.instrument = &g_instrument;
    ctx.sysex = &g_sysex;
    ctx.steppers = &g_steppers;
    ctx.servos = &g_servos;
    ctx.net = &g_net;
    ctx.safety = &g_safety;
    ctx.storage = &g_storage;
    // Every mutating callback below only ENQUEUES a command; loop() executes it.
    // The returned bool means "accepted into the queue", not "already done".
    // STOP is never enqueued (it must not be lost behind a full queue): it sets a
    // lock-free flag loop() honours first. Setting the flag always succeeds, so
    // /api/panic can truthfully report success.
    ctx.onPanic = []() { g_panicRequested.store(true); };
    ctx.onTestNote = [](uint8_t channel, uint8_t note, uint8_t vel,
                        uint16_t durationMs) -> uint32_t {
        AppCommand c{CmdType::TestNote};
        c.channel = channel; c.note = note; c.velocity = vel;
        c.durationMs = durationMs;
        return enqueueCommand(c);
    };
    ctx.onTestServo = [](int index, bool active) -> uint32_t {
        AppCommand c{CmdType::TestServo};
        c.servoIndex = static_cast<int16_t>(index);
        c.servoActive = active;
        return enqueueCommand(c);
    };
    ctx.commandState = [](uint32_t id) -> std::string { return commandStateStr(id); };
    // Storage reformat runs in the web task under the storage lock (loop() never
    // touches LittleFS, so this can't stall the safety loop).
    ctx.onFormatStorage = []() -> bool { return g_storage.format(); };
    // Read-only web handlers hold this around their reads so a reload in loop()
    // is never observed half-applied.
    ctx.lockState = []() { if (g_stateMutex) xSemaphoreTake(g_stateMutex, portMAX_DELAY); };
    ctx.unlockState = []() { if (g_stateMutex) xSemaphoreGive(g_stateMutex); };
    // Storage lock is DISTINCT from the state lock: loop() never takes it, so a
    // long LittleFS write from the web task can't block the safety loop (P0-1).
    ctx.lockStorage = []() { if (g_storageMutex) xSemaphoreTake(g_storageMutex, portMAX_DELAY); };
    ctx.unlockStorage = []() { if (g_storageMutex) xSemaphoreGive(g_storageMutex); };
    ctx.onSetWifi = [](bool hasSta, const std::string& sta, bool hasAp,
                       const std::string& ap) {
        Preferences p;
        p.begin("gmb", false);
        if (hasSta) p.putString("wifipass", String(sta.c_str()));  // only overwrite
        if (hasAp) p.putString("appass", String(ap.c_str()));      // provided fields
        p.end();
    };
    // Write-route authentication: an admin token stored in NVS. Until one is set
    // (first-run bootstrap) writes are allowed; once set, the X-GMB-Token header
    // must match. Panic stays unauthenticated (safety).
    ctx.checkToken = [](const std::string& provided) -> bool {
        Preferences p; p.begin("gmb", true);
        String stored = p.getString("admintoken", ""); p.end();
        if (stored.length() == 0) return true;
        return provided == std::string(stored.c_str());
    };
    ctx.onSetAdminToken = [](const std::string& t) {
        Preferences p; p.begin("gmb", false);
        p.putString("admintoken", String(t.c_str())); p.end();
    };
    ctx.authConfigured = []() -> bool {
        Preferences p; p.begin("gmb", true);
        String stored = p.getString("admintoken", ""); p.end();
        return stored.length() > 0;
    };
    ctx.appState = []() -> std::string {
        if (g_phase == AppPhase::Ready) return g_degraded ? "readyDegraded" : "ready";
        return g_phase == AppPhase::Homing ? "homing" : "boot";
    };
    ctx.readyStrings = []() -> int {
        if (g_phase != AppPhase::Ready) return 0;
        int n = 0;
        for (size_t i = 0; i < g_homing.size(); ++i)
            if (g_anchored[i] && !g_homing[i].failed()) ++n;
        return n;
    };
    ctx.onActivateProfile = [](const Profile& p) -> uint32_t {
        // Validate synchronously (pure, safe off the main loop) so an invalid
        // profile is rejected immediately; enqueue the actual apply for loop().
        if (!ProfileValidator::isActivatable(p)) return 0u;
        AppCommand c{CmdType::ActivateProfile};
        c.profile = new Profile(p);  // ownership transfers to the queued command
        return enqueueCommand(c);
    };
    ctx.onReset = []() -> uint32_t { return enqueueCommand(AppCommand{CmdType::Reset}); };
    g_web.begin(ctx, 80);

    // Home every axis before allowing play; unhomed axes never move for notes.
    // beginHoming() itself refuses if the profile is invalid or a channel failed
    // to attach, leaving the system safely in Boot.
    beginHoming(millis());
    g_web.refreshStatus();  // seed the cached status before the first GET
}

void loop() {
    uint32_t nowUs = micros();
    uint32_t nowMs = millis();

    g_net.tick(nowMs);
    g_steppers.updateSensors(nowMs);  // debounce HOME/LIMIT before any read

    // SAFETY FIRST, before any queued command runs this tick:
    // 1) Hardware E-stop (active-low). Must assert IMMEDIATELY: trip on the raw
    //    press this instant (a false trip only fails safe); the debounced level
    //    only filters the RELEASE so contact bounce can't un-latch it. (Still a
    //    software safety, not a substitute for a hardware cut of ENABLE / power.)
    if (g_estopPin >= 0) {
        bool rawPressed = digitalRead(g_estopPin) == LOW;
        bool debouncedPressed = !g_estopDeb.update(nowMs, digitalRead(g_estopPin) == HIGH);
        if ((rawPressed || debouncedPressed) &&
            g_safety.state() != SafetyState::EmergencyStop) {
            doEmergencyStop();
            purgeCommands();  // drop anything queued behind the E-stop
        }
    }
    // 2) Web/CC STOP flag: honoured before draining, and it purges the queue.
    bool panicked = servicePanic(nowMs);

    // Apply a BOUNDED number of queued web commands (skip if we just stopped, so
    // no stale activation/test runs after a STOP).
    if (!panicked) drainCommands(nowMs);
    servicePendingActivation(nowMs);  // phase 2 of a deferred profile activation

    // Wi-Fi loss policy (cahier des charges §21.4, default): cancel pending
    // commands and release notes in a controlled way, but stay armed/READY.
    static bool wasConnected = false;
    bool nowConnected = g_net.connected() && !g_net.accessPointActive();
    if (wasConnected && !nowConnected && g_phase == AppPhase::Ready) {
        g_instrument.panic();  // flushes queue + releases active notes
        g_steppers.stopAll();  // and stop any carriage still in motion
        g_safety.recordFault("wifi", "Wi-Fi link lost — pending commands cancelled",
                             nowMs);
    }
    wasConnected = nowConnected;

    // Deliver any scheduled test-note Note Offs that are due.
    for (size_t k = 0; k < g_testOffs.size();) {
        if ((int32_t)(nowMs - g_testOffs[k].atMs) >= 0) {
            MidiEvent off;
            off.type = static_cast<uint8_t>(MidiType::NoteOff);
            off.channel = g_testOffs[k].channel; off.data1 = g_testOffs[k].note;
            off.timestampUs = nowUs;
            g_instrument.handleEvent(off, nowUs);
            g_testOffs.erase(g_testOffs.begin() + k);
        } else {
            ++k;
        }
    }

    // Ingest Wi-Fi MIDI (bounded per tick). SysEx is always answered to its own
    // sender; notes only play once Ready.
    g_midi.poll(nowUs);
    for (auto& e : g_midi.events()) {
        g_web.broadcastMidi(e);  // feed the Web MIDI monitor (all phases)
        if (g_phase == AppPhase::Ready) g_instrument.handleEvent(e, nowUs);
    }
    for (auto& sx : g_midi.sysexPackets()) {
        // Serialise with the Web /api/sysex/request route (shared rate limiter +
        // snapshot); both take g_stateMutex.
        std::vector<uint8_t> resp;
        { StateGuard lock;
          resp = g_sysex.handleMessage(sx.bytes.data(), sx.bytes.size(), nowMs); }
        if (!resp.empty()) g_midi.reply(sx, resp.data(), resp.size());
    }
    g_midi.clear();

    g_instrument.tick(nowUs);   // flush chord groups
    g_servos.update(nowMs);     // scheduled servo returns / rest cut-off

    // Runtime PCA9685 health: a board lost AFTER arming (unplugged / brown-out)
    // means no finger/pluck can act — panic rather than keep "playing" blind.
    static uint32_t lastPcaCheckMs = 0;
    if (g_phase == AppPhase::Ready && nowMs - lastPcaCheckMs >= 500) {
        lastPcaCheckMs = nowMs;
        if (!g_servos.pcaHealthy()) {
            g_safety.recordFault("servo", "PCA9685 stopped responding on I2C", nowMs);
            doPanic();
        }
    }

    if (g_phase == AppPhase::Homing) {
        doHoming(nowMs);
        g_steppers.tick(nowUs);
    } else if (g_phase == AppPhase::Ready && g_safety.actuatorsAllowed()) {
        for (size_t i = 0; i < g_instrument.stringCount(); ++i) tickString(i, nowMs);
        g_steppers.tick(nowUs);
    }

    static uint32_t lastStatusMs = 0;
    if (nowMs - lastStatusMs >= 100) {
        lastStatusMs = nowMs;
        g_web.refreshStatus();     // rebuild the cached snapshot from live state...
        g_web.broadcastStatus();   // ...then push it (GET /api/status serves it too)
    }
}

#endif  // ARDUINO
