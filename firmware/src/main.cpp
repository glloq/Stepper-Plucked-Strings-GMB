// Stepper-Plucked-Strings-GMB — ESP32-S3 firmware entry point.
//
// Wires the pure-core logic (src/core/) to the ESP32 platform adapters
// (src/platform/esp32/). The core is unit-tested on the host; this file is the
// hardware integration and runs only on device.
//
// Boot sequence (spec §21.1 / §13): power-on safe → validate
// profile → home every axis (non-blocking) → only then arm for play.
#if defined(ARDUINO)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cmath>
#include <vector>

#include "core/app/AppPhase.h"
#include "core/app/Readiness.h"
#include "core/configuration/Profile.h"
#include "core/configuration/DeviceInstrument.h"
#include "core/configuration/ProfileActivation.h"
#include "core/configuration/ProfileValidator.h"
#include "core/diagnostics/Diagnostics.h"
#include "core/gmb/GmbSysExService.h"
#include "core/instrument/ActuatorManager.h"
#include "core/instrument/ActuatorResult.h"
#include "core/instrument/InstrumentController.h"
#include "core/midi/MidiEvent.h"
#include "core/motion/HomingController.h"
#include "core/safety/EstopPolarity.h"
#include "core/safety/SafetyManager.h"
#include "core/util/CommandResultRing.h"
#include "core/util/HoldButton.h"
#include "platform/esp32/MidiDinTransport.h"
#include "platform/esp32/MidiUsbTransport.h"
#include "platform/esp32/MidiWifi.h"
#include "platform/esp32/Net.h"
#include "platform/esp32/CommandDispatcher.h"
#include "platform/esp32/PlaybackScheduler.h"
#include "platform/esp32/ProfileStorage.h"
#include "platform/esp32/SafetySupervisor.h"
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
MidiUsbTransport g_usbMidi;  // native USB-MIDI (S3) — P1.7 skeleton, inert until wired
MidiDinTransport g_dinMidi;  // DIN-5/TRS UART — P1.7 functional, inert until a RX pin is bound
// Every transport feeds the SAME InstrumentController (P1.7): adding a transport
// never touches the instrument logic, and MidiEvent.source keeps the inputs apart.
MidiTransport* const g_transports[] = {&g_midi, &g_usbMidi, &g_dinMidi};
// Messages decoded per transport since boot, in g_transports order. `/api/status`
// reports these so "which MIDI input is actually feeding the instrument?" has a
// measured answer rather than an assumed one.
std::atomic<uint32_t> g_transportEvents[3]{};
// millis() of each transport's most recent decoded message (0 = never heard from),
// and which one it was. "What is playing this instrument right now" is a different
// question from "which has carried the most traffic since boot", and it is the one
// the status page is actually asked.
std::atomic<uint32_t> g_transportLastMs[3]{};
std::atomic<int> g_lastTransport{-1};
constexpr uint16_t kMidiUdpPort = 5006;  // AppleMIDI-free raw UDP MIDI (spec §8.1)
WebApi g_web;

// AppPhase + its wire labels now live in core/app/AppPhase.h (host-tested, P2.17).
AppPhase g_phase = AppPhase::Boot;
bool g_degraded = false;  // Ready but with one or more axes disabled by a fault

// In-rush governor for every actuator start (audit P1.6). Finger presses, strum-lift
// engagements and carriage repositioning are STAGGERABLE and go through it; the
// pluck/strum strikes that carry the sound are DEADLINE and are never throttled.
ActuatorManager g_actuators;

// Runtime telemetry (audit P2.19), served by GET /api/diagnostics.
Diagnostics g_diag;

// Pre-homing finger-lift: homing must not move a carriage while a finger is still
// pressed on the string. beginHoming() commands every finger to rest and waits
// until this deadline before the seek starts.
bool g_homingStarted = false;
// Web command awaiting the CURRENT profile activation's real outcome (audit 7):
// its result stays "running" from the swap until the new profile reaches Ready.
uint32_t g_pendingActivationCmd = 0;

// Two-phase profile activation: the OLD profile's fingers are driven to rest and
// allowed to lift BEFORE the old servo config is destroyed, so a profile that
// removes/reassigns a finger servo can't leave a finger pressed while the new
// profile homes. Owned by ProfileActivation (RAII, host-tested — P2.17).
ProfileActivation g_activation;

std::vector<HomingController> g_homing;
std::vector<bool> g_anchored;
std::vector<bool> g_axisFaulted;  // runtime fault (homing fail, LIMIT, etc.)

// BOOT button (GPIO0) forces the Wi-Fi hotspot (AP + captive portal) on a long
// press, so a wrong station config can never lock the user out. GPIO0 is the
// universal ESP32 dev-board BOOT button — held LOW while pressed (INPUT_PULLUP).
// A ~2 s hold avoids accidental triggers; the switch is live (no reboot).
constexpr uint8_t kBootButtonPin = 0;
constexpr uint32_t kBootHoldMs = 2000;
HoldButton g_bootHold;  // long-press on BOOT -> force hotspot (host-tested, P2.17)
std::atomic<bool> g_hotspotRequested{false};   // BOOT button / web -> force AP
std::atomic<bool> g_wifiScanRequested{false};  // GET /api/wifi/scan?start=1
// POST /api/midi/source. The web task must not touch the MIDI transport, so it
// only records the request; loop() applies it. -1 = no policy change pending.
std::atomic<int> g_midiSourceRequested{-1};
std::atomic<bool> g_midiUnlockRequested{false};
// POST /api/wifi with apply:true. The web task must never touch the radio, so it
// only raises this flag; loop() (which owns Net) does the reconfiguration.
std::atomic<bool> g_netApplyRequested{false};
std::string g_wifiScanJson =                   // guarded by g_stateMutex
    "{\"ok\":true,\"scanning\":false,\"networks\":[]}";
uint32_t g_seenScanGeneration = 0;

int8_t g_estopPin = -1;
Debouncer g_estopDeb;  // debounced E-stop input (avoids a spurious trip)

// Pending test-note Note Offs (scheduled by /api/test/note). A small queue so a
// second test before the first ends does not drop the first note's release.
struct TestNoteOff { uint8_t channel; uint8_t note; uint32_t atMs; };
std::vector<TestNoteOff> g_testOffs;

// Per-string playback FSM (StringSched, estimateMoveMs, the mechanical sequence)
// now lives in platform/esp32/PlaybackScheduler.h.
PlaybackScheduler g_scheduler;
// Arming / homing / hard stop / panic / runtime faults (SafetySupervisor.h).
SafetySupervisor g_supervisor;

// ---- Web -> loop() command queue (P0: no mechanical state off the main loop) --
//
// ESPAsyncWebServer runs its callbacks in the AsyncTCP task, which can execute
// in parallel with loop(). If those callbacks touched g_profile / g_instrument /
// g_steppers / g_servos directly they could reallocate a std::vector while
// loop() is iterating it. Instead every mutating request only ENQUEUES a command
// here; loop() is the SOLE owner of the mechanical state and drains the queue
// sequentially. Read-only handlers take g_stateMutex so a reallocation in loop()
// (profile reload, capability rebuild) can never be seen half-done.
// CmdType / AppCommand / the queue + result ring now live in
// platform/esp32/CommandDispatcher.h.
CommandDispatcher g_commands;

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

// RAM cache of "an admin token is configured" so the 100 ms status rebuild never
// opens NVS (audit P1-10). Seeded at setup, updated on /api/auth.
bool g_authConfiguredCache = false;

// Enqueue a command (called from the AsyncTCP task). Returns false if the queue
// is full so the caller can report back-pressure instead of silently dropping.
// Returns the assigned command id (0 if the queue is full / unavailable).
uint32_t enqueueCommand(const AppCommand& in) {
    uint32_t id = g_commands.enqueue(in);
    g_diag.observeCmdQueueDepth(g_commands.depth());
    return id;
}

void setCommandResult(uint32_t id, uint8_t state) { g_commands.finish(id, state); }
std::string commandStateStr(uint32_t id) { return g_commands.commandState(id); }

// RAII guard for the shared-state mutex (used by loop() around reloads and by the
// read-only web handlers around their reads).
struct StateGuard {
    StateGuard() { if (g_stateMutex) xSemaphoreTake(g_stateMutex, portMAX_DELAY); }
    ~StateGuard() { if (g_stateMutex) xSemaphoreGive(g_stateMutex); }
};

// Cached /api/diagnostics body. Rebuilt ON THE LOOP and published under the state
// mutex; the web task then serves an immutable copy (the counters are plain
// uint32_t/bool the loop mutates, so the async task must never serialise them live).
std::string g_diagJson = "{}";

void hardStopAll();  // defined below; needed by faultRuntimeAxis
void faultRuntimeAxis(size_t i, const char* reason, uint32_t nowMs);
void doPanic();      // defined below; needed by serviceLostPcaBoard
void bindDinMidi();  // defined below; needed by servicePendingActivation

// Route an ActuatorResult from a scheduler-facing command (audit P1.4): Ok passes
// through, anything else faults the axis with the reason spelled out and returns
// false so the caller aborts this tick's musical step. `what` names the command
// site ("finger press", "carriage move", ...).
bool actOk(size_t axis, ActuatorResult r, const char* what, uint32_t nowMs) {
    if (ok(r)) return true;
    faultRuntimeAxis(axis, (std::string(what) + " refused (" + actuatorResultName(r) + ")").c_str(),
                     nowMs);
    return false;
}

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
    g_scheduler.configure(g_profile.strings.size());

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
    // Bus 0 = Wire (SDA/SCL, /OE on SERVO_OE); bus 1 = Wire1 (SDA2/SCL2), with its
    // own /OE when SERVO_OE2 is assigned, otherwise sharing the single /OE line.
    g_servos.begin(g_profile.servos, pinOf("SDA"), pinOf("SCL"), pinOf("SERVO_OE"),
                   pinOf("SDA2"), pinOf("SCL2"), pinOf("SERVO_OE2"));
    // A scheduled servo write that fails has no caller to check it (the automatic
    // post-strike return): route it to the owning string's fault path (audit P1.4).
    g_servos.onUpdateFault([](int servo, ActuatorResult r) {
        int si = g_servos.stringIndexOf(servo);
        if (si >= 0) faultRuntimeAxis(static_cast<size_t>(si),
                                      (std::string("servo return failed (") +
                                       actuatorResultName(r) + ")").c_str(), millis());
    });
    // In-rush caps for every staggerable start (finger press, strum lift, carriage
    // repositioning) — the sound-carrying strikes bypass the governor (P1.6).
    g_actuators.configure(g_profile.power.maxConcurrentMoves,
                          g_profile.power.maxConcurrentPerBoard,
                          g_profile.power.staggerMs);
    g_actuators.reset();

    g_homing.assign(g_profile.strings.size(), HomingController{});
    g_anchored.assign(g_profile.strings.size(), false);
    g_axisFaulted.assign(g_profile.strings.size(), false);
    for (size_t i = 0; i < g_profile.strings.size(); ++i) {
        if (i < g_profile.homing.size()) g_homing[i].configure(g_profile.homing[i]);
    }

    // NOTE (audit P1-7): the network stack (Net) is deliberately NOT restarted
    // here. Applying a new SSID / mode mid-session would drop the very connection
    // used to configure the device; Wi-Fi and static-IP changes therefore take
    // effect on the next reboot (the /api/wifi route already reports this). The
    // status DTO exposes both the active mode (Net) and the profile's configured
    // SSID so the UI can prompt a reboot when they differ.

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
    // The ready/degraded rule itself is host-tested in core/app/Readiness.h (P2.17):
    // an axis is ready only if the profile enabled it AND no runtime fault hit it.
    Readiness r = applyRuntimeFaults(rp, g_axisFaulted);
    g_degraded = r.degraded;
    g_profile.capabilitiesRevision++;
    rp.capabilitiesRevision = g_profile.capabilitiesRevision;
    g_sysex.rebuild(rp);
    return r.ready;
}

// Push a spontaneous "capabilities changed" SysEx to the last host that queried
// us, so General-Midi-Boop learns of a runtime change without polling. GMB v2
// block 0x11 change flags: bit 1 = INSTRUMENTS_CHANGED (the capability set /
// string config moved). GMB uses it only as a cue to re-read the handshake and
// compare the revision, so an approximate flag is fine.
void notifyCapabilitiesChanged() {
    if (!g_midi.hasLastSender()) return;
    std::vector<uint8_t> msg = g_sysex.notification(0x02);
    if (!msg.empty()) g_midi.notifyLastSender(msg.data(), msg.size());
}

// The safety operations (fault path, E-stop polarity, homing, hard stop, panic)
// now live in platform/esp32/SafetySupervisor.h. These free functions remain as
// thin delegates so every call site below is unchanged.
void faultRuntimeAxis(size_t i, const char* reason, uint32_t nowMs) {
    g_supervisor.faultRuntimeAxis(i, reason, nowMs);
}
bool estopAsserted(bool pinHigh) { return g_supervisor.estopAsserted(pinHigh); }
bool estopAssertedNow() { return g_supervisor.estopAssertedNow(); }
void serviceLostPcaBoard(uint8_t failedBoard, uint32_t nowMs) {
    g_supervisor.serviceLostPcaBoard(failedBoard, nowMs);
}
bool safetyLocked() { return g_supervisor.locked(); }
bool beginHoming(uint32_t nowMs) { return g_supervisor.beginHoming(nowMs); }
bool doReset(uint32_t nowMs) { return g_supervisor.reset(nowMs); }
void doHoming(uint32_t nowMs) { g_supervisor.tickHoming(nowMs); }
void hardStopAll() { g_supervisor.hardStopAll(); }
void doPanic() { g_supervisor.panic(); }
void doEmergencyStop() { g_supervisor.emergencyStop(); }

// ---- loop-side command handlers (only ever called from drainCommands) --------

// Phase 1 of activation: validate, bring the carriages to a CONTROLLED stop and
// drive the OLD profile's servos to rest (a controlled park — never a hard stop:
// nothing is broken, so the mechanics come home properly). The teardown +
// reconfigure + re-home is deferred until the old fingers have physically lifted
// (servicePendingActivation).
bool doActivateProfile(const Profile& p, uint32_t nowMs, uint32_t commandId) {
    if (!ProfileValidator::isActivatable(p)) return false;
    // Refuse to (re)start motion while a panic / E-stop is latched; the user must
    // explicitly reset first (POST /api/reset).
    if (safetyLocked()) return false;
    // Release the current notes and ramp the carriages down under control, but KEEP
    // both the drivers and the servo outputs powered so the old fingers can still be
    // driven up (audit P0.3: hardStop is for E-stop, controlledPark for a swap).
    g_instrument.panic();
    g_steppers.controlledStopAll();
    g_scheduler.reset();
    g_testOffs.clear();  // a profile change cancels any pending test Note Offs
    // Immediately demote from Armed to PowerOnSafe and enter Reconfiguring so no
    // test servo / test note / MIDI can drive the (about-to-be-torn-down) hardware
    // during the release+rebuild window (audit P0-2). Only a panic/E-stop or the
    // internal hard stop may act until homing re-arms.
    g_safety.reset();
    g_phase = AppPhase::Reconfiguring;

    // Controlled park of every current servo before the old config is destroyed.
    // The rest command is CHECKED: a refused write means the park cannot be trusted,
    // so the swap is abandoned rather than tearing down a config whose finger may
    // still be pressed on a string (audit P1 — a park used to be fire-and-forget).
    g_servos.outputEnable(true);
    ServoBank::ParkResult park = g_servos.moveAllToRest();
    if (!park.ok) {
        hardStopAll();
        g_safety.recordFault(
            "park", std::string("servo ") + std::to_string(park.failedServo) +
                        " refused its rest command (" + actuatorResultName(park.reason) +
                        ") — profile activation aborted", nowMs);
        return false;
    }
    // Wait for the slower of the two mechanical settles: the servos travelling to
    // rest, and the carriages decelerating to a stand-still.
    uint32_t wait = g_servos.parkDurationMs();
    uint32_t stopWait = g_steppers.stopDurationMs();
    if (stopWait > wait) wait = stopWait;
    g_activation.begin(p, nowMs + wait, commandId);
    // The command's outcome is DEFERRED: it is only "succeeded" once the new profile
    // really reaches Ready (audit 7), so report it as running for now.
    setCommandResult(commandId, CommandResultRing::Running);
    return true;
}

// Phase 2: once the old fingers have lifted, tear down the old profile, swap in
// the new one and re-home. Runs from loop().
void servicePendingActivation(uint32_t nowMs) {
    uint32_t cid = g_activation.commandId();
    if (!g_activation.service(nowMs, [&](const Profile& p) {
            g_servos.hardStop();          // now safe to cut the old servo outputs
            g_steppers.enableDrivers(false);
            StateGuard lock;  // atomic profile swap + runtime rebuild (P0-2)
            g_profile = p;
            g_profile.capabilitiesRevision++;
            applyProfile();  // re-resolves pins incl. ESTOP, reinitialises hardware
            bindDinMidi();   // the MIDI_RX pin may have moved with the new config
        }))
        return;
    // Carry the awaiting command across the re-home: doHoming marks it succeeded
    // when the NEW profile actually reaches Ready.
    g_pendingActivationCmd = cid;
    // Re-home after the mechanical change (§16). If it cannot start (attach fault,
    // invalid config), stay safely in Boot/PowerOnSafe rather than stuck armed.
    if (!beginHoming(nowMs)) {
        g_phase = AppPhase::Boot;
        if (cid) setCommandResult(cid, CommandResultRing::Failed);
        g_pendingActivationCmd = 0;
    }
}

// Web test note: only when Ready, and only if we can guarantee its Note Off.
bool doTestNote(uint8_t channel, uint8_t note, uint8_t vel, uint16_t durationMs,
                uint32_t nowMs, int16_t ccStringValue, int16_t ccFretValue) {
    if (g_phase != AppPhase::Ready) return false;
    // Reject out-of-range MIDI values: a 4-bit channel and 7-bit note/velocity.
    // (Defence at the source; the selector also masks the channel.)
    if (channel > 15 || note > 127 || vel > 127) return false;
    if (ccStringValue > 127 || ccFretValue > 127) return false;
    if (durationMs > 10000) durationMs = 10000;  // cap a runaway hold
    // Never emit a Note On we cannot later release: refuse if the deferred
    // Note-Off queue is full (would otherwise leave the note stuck on).
    if (g_testOffs.size() >= 16) return false;

    // Optional selection CCs, emitted through the SAME path a controller's would
    // take (§16). Without them the test bypassed the selector entirely, so it
    // exercised note allocation but proved nothing about the General-Midi-Boop
    // string/fret mechanism it claimed to be testing. Values are what a
    // controller sends on the wire; the selector applies numbering, offset,
    // reverse order and the mapping table itself — so a wrong mapping shows up
    // here instead of being papered over.
    auto sendCc = [&](uint8_t ccNumber, uint8_t value) {
        MidiEvent cc;
        cc.type = static_cast<uint8_t>(MidiType::ControlChange);
        cc.channel = channel;
        cc.data1 = ccNumber;
        cc.data2 = value;
        cc.timestampUs = micros();
        cc.source = static_cast<uint8_t>(MidiSource::WebUiTest);
        g_instrument.handleEvent(cc, cc.timestampUs);
    };
    const SelectorConfig& sel = g_profile.selector;
    if (ccStringValue >= 0) sendCc(sel.string.ccNumber, static_cast<uint8_t>(ccStringValue));
    if (ccFretValue >= 0) sendCc(sel.fret.ccNumber, static_cast<uint8_t>(ccFretValue));

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

// Web jog: nudge one axis by a small signed delta (manual bring-up, checking the
// motor direction, positioning for fret calibration). Only when Ready, actuators
// armed, the axis homed & not faulted, and idle (no live note) so it can never
// fight the playback scheduler. moveToMm clamps to the axis travel.
// Shared gate for every OPERATOR-driven axis move (jog, go-to-position). The
// conditions are the same whichever way the target is expressed, and they must
// stay the same: the difference between the two commands is arithmetic, not
// safety.
bool axisManuallyMovable(int axis, uint32_t nowMs) {
    if (g_phase != AppPhase::Ready) return false;
    if (!g_safety.actuatorsAllowed()) return false;
    if (axis < 0 || axis >= static_cast<int>(g_instrument.stringCount())) return false;
    if (axis < static_cast<int>(g_profile.strings.size()) &&
        !g_profile.strings[axis].enabled) return false;         // disabled axis: no-op
    if (axis < static_cast<int>(g_axisFaulted.size()) && g_axisFaulted[axis]) return false;
    if (g_instrument.target(axis).active) return false;         // don't fight a live note
    if (!g_scheduler.idle(axis)) return false;                   // axis busy
    if (g_steppers.isRunning(axis)) return false;                // still moving
    // Wait until a just-released finger has fully lifted (§16: no drag).
    if (!g_scheduler.jogSafe(axis, nowMs)) return false;
    return true;
}

bool doJog(int axis, double deltaMm, uint32_t nowMs) {
    if (!axisManuallyMovable(axis, nowMs)) return false;
    if (deltaMm > 25.0) deltaMm = 25.0;                          // bound one nudge
    if (deltaMm < -25.0) deltaMm = -25.0;
    g_steppers.moveToMm(axis, g_steppers.positionMm(axis) + deltaMm);
    return true;
}

// Web "go to this position": move one axis to an ABSOLUTE target measured from
// the homing zero. Fret calibration needs this — reaching fret 9 by summing jogs
// accumulates every rounding error and every refused nudge into the position the
// operator is about to record as ground truth. moveToMm clamps to the axis
// travel, so an out-of-range target parks at the limit instead of being refused
// silently.
bool doMoveTo(int axis, double positionMm, uint32_t nowMs) {
    if (!axisManuallyMovable(axis, nowMs)) return false;
    g_steppers.moveToMm(axis, positionMm);
    return true;
}

// Discard every queued command without executing it (used after a panic so a
// stale profile activation / test can't fire once the STOP has latched).
void purgeCommands() { g_commands.purge(); }

// Honour a pending STOP before anything else. Returns true if a panic ran, so
// the caller can skip the rest of this tick's command/motion work.
bool servicePanic(uint32_t nowMs) {
    (void)nowMs;
    if (!g_panicRequested.exchange(false)) return false;
    doPanic();
    purgeCommands();  // drop anything queued behind the STOP
    return true;
}

// Run one drained command. A profile activation is DEFERRED: doActivateProfile
// already marked it Running and the supervisor closes it when the new profile
// really reaches Ready — reporting "succeeded" here would mean "started" (audit 7).
CmdOutcome runCommand(const AppCommand& c, uint32_t nowMs) {
    switch (c.type) {
        case CmdType::Panic: doPanic(); purgeCommands(); return CmdOutcome::Succeeded;
        case CmdType::Reset: return doReset(nowMs) ? CmdOutcome::Succeeded : CmdOutcome::Refused;
        case CmdType::ActivateProfile:
            return (c.profile && doActivateProfile(*c.profile, nowMs, c.id))
                       ? CmdOutcome::Deferred : CmdOutcome::Refused;
        case CmdType::TestNote:
            return doTestNote(c.channel, c.note, c.velocity, c.durationMs, nowMs,
                              c.ccStringValue, c.ccFretValue)
                       ? CmdOutcome::Succeeded : CmdOutcome::Refused;
        case CmdType::TestServo:
            return doTestServo(c.servoIndex, c.servoActive) ? CmdOutcome::Succeeded
                                                            : CmdOutcome::Refused;
        case CmdType::Jog:
            return doJog(c.axisIndex, c.jogDeltaMm, nowMs) ? CmdOutcome::Succeeded
                                                           : CmdOutcome::Refused;
        case CmdType::MoveTo:
            return doMoveTo(c.axisIndex, c.targetMm, nowMs) ? CmdOutcome::Succeeded
                                                            : CmdOutcome::Refused;
    }
    return CmdOutcome::Refused;
}

void drainCommands(uint32_t nowMs) {
    static constexpr int kMaxCommandsPerTick = 2;
    g_commands.drain(kMaxCommandsPerTick,
                     [nowMs](const AppCommand& c) { return runCommand(c, nowMs); });
}

// The per-string striker: the plectrum ('pluck') if present, otherwise the
// per-string strum servo ('strum'). Both name the same physical per-string
// striker, so an instrument may wire either one. There is no shared strummer —
// every string is plucked/strummed on its own.
// Reset reason as a short stable string, so a bench run can tell a clean power-up
// from a brown-out or a watchdog reset.
const char* resetReasonStr() {
#if defined(ARDUINO)
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "powerOn";
        case ESP_RST_EXT:      return "external";
        case ESP_RST_SW:       return "software";
        case ESP_RST_PANIC:    return "panic";
        case ESP_RST_INT_WDT:  return "intWdt";
        case ESP_RST_TASK_WDT: return "taskWdt";
        case ESP_RST_WDT:      return "wdt";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_DEEPSLEEP:return "deepSleep";
        case ESP_RST_SDIO:     return "sdio";
        default:               return "unknown";
    }
#else
    return "unknown";
#endif
}

std::string buildDiagnosticsJson() {
    const DiagCounters& c = g_diag.counters();
    JsonDocument doc;
    doc["uptimeMs"] = millis();
    doc["resetReason"] = resetReasonStr();
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["minFreeHeap"] = ESP.getMinFreeHeap();
    doc["state"] = appPhaseName(g_phase, g_degraded);
    JsonObject midi = doc["midi"].to<JsonObject>();
    midi["events"] = c.midiEvents;
    midi["droppedEvents"] = c.midiDropped;
    midi["droppedPackets"] = c.udpDropped;
    midi["rejectedPackets"] = c.udpRejected;  // refused by the UDP source gate (P1.11)
    JsonObject sched = doc["scheduler"].to<JsonObject>();
    sched["maxLatencyUs"] = c.schedulerMaxLatencyUs;
    sched["jitterUs"] = c.schedulerJitterUs;
    sched["meanUs"] = c.schedulerMeanUs;
    doc["cmdQueueHighWater"] = c.cmdQueueHighWater;
    doc["faults"] = c.faults;
    doc["servoMoves"] = c.servoMoves;
    doc["governorThrottles"] = c.governorThrottles;
    // Motion-side counters: only this build has carriages to lose (P2.19).
    JsonObject motion = doc["motion"].to<JsonObject>();
    motion["axisMoves"] = c.axisMoves;
    motion["homingFailures"] = c.homingFailures;
    motion["limitTrips"] = c.limitTrips;
    motion["moveTimeouts"] = c.moveTimeouts;
    JsonObject mv = doc["moveMix"].to<JsonObject>();  // P1.6 in-rush picture
    mv["deadline"] = c.deadlineMoves;                 // sound strikes (never throttled)
    mv["staggerableGranted"] = c.staggerableGrants;   // positioning starts granted
    mv["staggerableDeferred"] = c.governorThrottles;  // positioning starts deferred
    doc["wifiReconnects"] = c.wifiReconnects;
    JsonObject pca = doc["pca"].to<JsonObject>();
    pca["used"] = c.pcaUsed;
    pca["healthy"] = c.pcaHealthy;
    if (c.pcaFailedBoard != 0xFF)
        pca["failedBoard"] = ServoBank::boardName(c.pcaFailedBoard);
    std::string out;
    serializeJson(doc, out);
    return out;
}

// Force the Wi-Fi hotspot (AP + captive portal) from a BOOT-button long-press or a
// web "Start hotspot" request. Runs on the main loop (owns Net + WiFi). Switching
// the radio is independent of the instrument state, so it works in any phase — the
// point is precisely to stay reachable when the machine will not arm.
// ---- DIN-5 / TRS MIDI input -----------------------------------------------
//
// DIN MIDI is just 31250-baud serial, and MidiDinTransport has always been able
// to decode it — it was passed a null Stream, so the whole path was dead. What it
// needed was a device-level pin, which now exists as the `MIDI_RX` signal.
//
// UART2 is used because UART0 is the programming/diagnostic console: binding MIDI
// to it would fight the serial monitor and eat the boot log. RX only — this
// firmware receives MIDI, it does not send it — so no TX pin is claimed.
constexpr int8_t kDinMidiUart = 2;
constexpr uint32_t kDinMidiBaud = 31250;
bool g_dinMidiBound = false;

void bindDinMidi() {
    const int8_t rx = pinOf("MIDI_RX");
    if (rx < 0) {
        g_dinMidi.begin(nullptr);   // no pin assigned: the transport stays inert
        g_dinMidiBound = false;
        return;
    }
    static HardwareSerial dinSerial(kDinMidiUart);
    // -1 for TX: claim only the RX pin, so nothing else is taken from the user.
    dinSerial.begin(kDinMidiBaud, SERIAL_8N1, rx, -1);
    g_dinMidi.begin(&dinSerial);
    g_dinMidiBound = true;
    Serial.printf("[midi] DIN input on GPIO%d (UART%d, %u baud)\n", rx,
                  static_cast<int>(kDinMidiUart),
                  static_cast<unsigned>(kDinMidiBaud));
}

// ---- device-level network settings (NVS) ---------------------------------
//
// The link config belongs to the DEVICE, not to the instrument (P1.13): moving a
// profile between machines must not carry one machine's SSID onto another, and
// changing instrument must not drop you off the network. It therefore lives in
// NVS beside the passwords and OVERRIDES whatever an older profile still carries.
// NVS keys are capped at 15 characters.
void loadNetworkOverrides(NetworkConfig& cfg) {
    Preferences p;
    p.begin("gmb", true);
    if (p.isKey("netmode")) {
        cfg.mode = p.getString("netmode", "accessPoint") == "station"
                       ? NetworkMode::Station : NetworkMode::AccessPoint;
        cfg.ssid = p.getString("netssid", cfg.ssid.c_str()).c_str();
        cfg.apSsid = p.getString("netapssid", cfg.apSsid.c_str()).c_str();
        cfg.hostname = p.getString("nethost", cfg.hostname.c_str()).c_str();
    }
    p.end();
}

void storeNetworkOverrides(const NetworkConfig& cfg) {
    Preferences p;
    p.begin("gmb", false);
    p.putString("netmode", cfg.mode == NetworkMode::Station ? "station" : "accessPoint");
    p.putString("netssid", String(cfg.ssid.c_str()));
    p.putString("netapssid", String(cfg.apSsid.c_str()));
    p.putString("nethost", String(cfg.hostname.c_str()));
    p.end();
}

// Reconfigure the radio after POST /api/wifi with apply:true. Runs on the main
// loop, which owns Net; Net::begin() resets the failure counters and any forced-
// hotspot latch, then re-runs the station attempt / AP with the usual automatic
// fallback to the hotspot — so a wrong SSID costs a fallback, not a lockout.
void serviceNetworkApply() {
    if (!g_netApplyRequested.exchange(false)) return;
    NetworkConfig cfg;
    { StateGuard lock; cfg = g_profile.network; }
    Preferences prefs;
    prefs.begin("gmb", true);
    String staPass = prefs.getString("wifipass", "");
    String apPass = prefs.getString("appass", "");
    prefs.end();
    g_net.begin(cfg, staPass.c_str(), apPass.c_str());
    Serial.println(F("web: Wi-Fi settings applied"));
}

void serviceHotspotRequests(uint32_t nowMs) {
    bool down = digitalRead(kBootButtonPin) == LOW;  // active-low BOOT button
    if (g_bootHold.update(down, nowMs)) g_hotspotRequested.store(true);  // long-press
    if (g_hotspotRequested.exchange(false)) {
        g_net.forceAccessPoint();
        Serial.println(F("BOOT/web: forced Wi-Fi hotspot (AP + captive portal)"));
    }
}

// Serialize the latest Wi-Fi scan state into the snapshot the web task serves
// (GET /api/wifi/scan reads it under the state lock — never the live vectors).
void refreshWifiScanJson() {
    JsonDocument doc;
    doc["ok"] = true;
    doc["scanning"] = g_net.scanInProgress();
    JsonArray arr = doc["networks"].to<JsonArray>();
    for (const auto& r : g_net.scanResults()) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = r.ssid;
        o["rssi"] = r.rssi;
        o["secure"] = r.secure;
        o["channel"] = r.channel;
    }
    std::string out;
    serializeJson(doc, out);
    StateGuard lock;
    g_wifiScanJson = out;
}

// Scan requests. Runs on the main loop (owner of Net/WiFi): the web handler only
// sets a flag, so the async task never touches the radio.
void serviceWifiRequests() {
    if (g_wifiScanRequested.exchange(false)) {
        g_net.startScan();
        refreshWifiScanJson();  // snapshot now says scanning:true
    }
    if (g_net.scanGeneration() != g_seenScanGeneration) {
        g_seenScanGeneration = g_net.scanGeneration();
        refreshWifiScanJson();  // fresh results landed
    }
}

void refreshDiagnosticsJson() {
    std::string out = buildDiagnosticsJson();  // built outside the lock
    StateGuard lock;
    g_diagJson = out;
}

}  // namespace

// Wire the REST/WebSocket API to the app. Every callback here is the SAME shape:
// a read-only getter takes the state mutex, and anything that MOVES the machine
// only enqueues a command — the web task never owns mechanical state (spec P0).
// Lifted out of setup() so the boot sequence reads as a sequence again.
WebContext buildWebContext() {
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
                        uint16_t durationMs, int ccString, int ccFret) -> uint32_t {
        AppCommand c{CmdType::TestNote};
        c.channel = channel; c.note = note; c.velocity = vel;
        c.durationMs = durationMs;
        c.ccStringValue = static_cast<int16_t>(ccString);
        c.ccFretValue = static_cast<int16_t>(ccFret);
        return enqueueCommand(c);
    };
    ctx.onTestServo = [](int index, bool active) -> uint32_t {
        AppCommand c{CmdType::TestServo};
        c.servoIndex = static_cast<int16_t>(index);
        c.servoActive = active;
        return enqueueCommand(c);
    };
    ctx.onJog = [](int axis, double deltaMm) -> uint32_t {
        AppCommand c{CmdType::Jog};
        c.axisIndex = static_cast<int16_t>(axis);
        c.jogDeltaMm = static_cast<float>(deltaMm);
        return enqueueCommand(c);
    };
    ctx.onMoveTo = [](int axis, double positionMm) -> uint32_t {
        AppCommand c{CmdType::MoveTo};
        c.axisIndex = static_cast<int16_t>(axis);
        c.targetMm = static_cast<float>(positionMm);
        return enqueueCommand(c);
    };
    ctx.commandState = [](uint32_t id) -> std::string { return commandStateStr(id); };
    ctx.diagnosticsJson = []() -> std::string { StateGuard lock; return g_diagJson; };
    ctx.midiSourcePolicy = []() -> std::string {
        return udpSourcePolicyName(g_midi.sourcePolicy());
    };
    ctx.midiSourceLocked = []() -> bool { return g_midi.sourceLocked(); };
    ctx.lastMidiSource = []() -> std::string {
        int i = g_lastTransport.load();
        if (i < 0) return "none";   // nothing received yet: do not name a guess
        return g_transports[i]->name();
    };
    ctx.lastMidiEventMs = []() -> uint32_t {
        int i = g_lastTransport.load();
        return i < 0 ? 0u : g_transportLastMs[i].load();
    };
    ctx.midiTransports = []() -> std::vector<WebContext::MidiTransportState> {
        std::vector<WebContext::MidiTransportState> out;
        WebContext::MidiTransportState udp;
        udp.name = "wifiUdp";
        udp.label = "Wi-Fi (UDP)";
        // Bound means "can actually receive": the socket is only useful with a link.
        udp.bound = g_net.connected();
        udp.detail = udp.bound ? ("UDP port " + std::to_string(kMidiUdpPort))
                               : "no network link";
        udp.events = g_transportEvents[0].load();
        udp.lastEventMs = g_transportLastMs[0].load();
        out.push_back(udp);

        WebContext::MidiTransportState usb;
        usb.name = "usb";
        usb.label = "USB-MIDI";
        usb.bound = g_usbMidi.bound();
        usb.detail = usb.bound ? "native USB-MIDI (TinyUSB), host connected"
#if defined(GMB_USB_MIDI)
                               : "native USB-MIDI built in — no host connected";
#else
                               : "not built in (see the esp32-s3-usbmidi env)";
#endif
        usb.events = g_transportEvents[1].load();
        usb.lastEventMs = g_transportLastMs[1].load();
        out.push_back(usb);

        WebContext::MidiTransportState din;
        din.name = "din";
        din.label = "DIN-5 / TRS";
        din.bound = g_dinMidiBound;
        din.detail = g_dinMidiBound
                         ? ("GPIO" + std::to_string(pinOf("MIDI_RX")) + ", UART" +
                            std::to_string(kDinMidiUart) + ", 31250 baud")
                         : "no MIDI_RX pin assigned";
        din.events = g_transportEvents[2].load();
        din.lastEventMs = g_transportLastMs[2].load();
        out.push_back(din);
        return out;
    };
    ctx.onSetMidiSource = [](int policy, bool unlock) -> bool {
        if (policy >= 0 && policy <= 2) {
            Preferences p;
            if (!p.begin("gmb", false)) return false;
            size_t written = p.putInt("midisrc", policy);
            p.end();
            if (written == 0) return false;   // NVS write failed: report, don't apply
            g_midiSourceRequested.store(policy);  // applied on the main loop
        }
        if (unlock) g_midiUnlockRequested.store(true);
        return true;
    };
    ctx.onStartHotspot = []() { g_hotspotRequested.store(true); };
    ctx.onWifiScanStart = []() { g_wifiScanRequested.store(true); };
    ctx.wifiScanJson = []() -> std::string { StateGuard lock; return g_wifiScanJson; };
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
    // Store the device's network settings and, when asked, apply them live. The
    // handler already validated the request; everything here is persistence plus
    // a flag, because the radio belongs to loop().
    ctx.onSetWifi = [](const WebContext::WifiRequest& rq) -> std::string {
        Preferences p;
        p.begin("gmb", false);
        // Only overwrite a password that was actually provided; erasing one is
        // explicit, so "leave blank to keep" and "really forget it" stay distinct.
        if (rq.hasStationPassword) p.putString("wifipass", String(rq.stationPassword.c_str()));
        else if (rq.clearStationPassword) p.remove("wifipass");
        if (rq.hasApPassword) p.putString("appass", String(rq.apPassword.c_str()));
        else if (rq.clearApPassword) p.remove("appass");
        p.end();
        if (rq.hasNetwork) {
            storeNetworkOverrides(rq.network);
            // Mirror into the running profile so /api/status, the exported profile
            // and the next boot all agree with what was just stored.
            StateGuard lock;
            g_profile.network = rq.network;
        }
        if (!rq.apply) return "stored; reboot to apply";
        g_netApplyRequested.store(true);
        return "applied now";
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
        g_authConfiguredCache = !t.empty();  // keep the RAM cache in step
    };
    // Cached in RAM: the status DTO is rebuilt every 100 ms and must NOT open NVS
    // that often. Seeded once here; refreshed only when the token is set (P1-10).
    { Preferences p; p.begin("gmb", true);
      g_authConfiguredCache = p.getString("admintoken", "").length() > 0; p.end(); }
    ctx.authConfigured = []() -> bool { return g_authConfiguredCache; };
    ctx.appState = []() -> std::string { return appPhaseName(g_phase, g_degraded); };
    ctx.readyStrings = []() -> int {
        if (g_phase != AppPhase::Ready) return 0;
        int n = 0;
        for (size_t i = 0; i < g_homing.size(); ++i)
            if (g_anchored[i] && !g_homing[i].failed()) ++n;
        return n;
    };
    ctx.onActivateProfile = [](const Profile& p, bool keepDeviceConfig) -> uint32_t {
        Profile target = p;
        if (keepDeviceConfig) {
            // Loading a stored INSTRUMENT: keep this machine's device half. The
            // whole profile used to be adopted, so a slot saved on (or before) a
            // different setup silently replaced the network settings, the pin map,
            // the E-stop polarity and the declared power hardware of the machine
            // actually running. The radio was not re-initialised, so /api/status
            // then reported a network the device was not on — and the E-stop
            // polarity change is worse than cosmetic.
            StateGuard lock;
            target = mergeProfile(deviceConfigOf(g_profile), instrumentProfileOf(p), p);
        }
        // Validate synchronously (pure, safe off the main loop) so an invalid
        // profile is rejected immediately; enqueue the actual apply for loop().
        // Validate the MERGED profile: the instrument half must fit THIS device's
        // pins, which is exactly the combination that will run.
        if (!ProfileValidator::isActivatable(target)) return 0u;
        AppCommand c{CmdType::ActivateProfile};
        c.profile = new Profile(target);  // ownership transfers to the queued command
        return enqueueCommand(c);
    };
    ctx.onReset = []() -> uint32_t { return enqueueCommand(AppCommand{CmdType::Reset}); };
    return ctx;
}

void setup() {
    Serial.begin(115200);
    g_safety.boot();  // drivers off, servos neutralised (spec §21.1)

    // Web -> loop() command channel + shared-state mutex, created before the web
    // server so the first request is already safe.
    g_commands.begin(16);
    g_stateMutex = xSemaphoreCreateMutex();
    g_storageMutex = xSemaphoreCreateMutex();

    pinMode(kBootButtonPin, INPUT_PULLUP);  // BOOT button -> force hotspot (long press)
    g_bootHold.configure(kBootHoldMs);

    // The playback FSM's collaborators never change identity, so bind once. The two
    // callbacks keep the fault path central: the scheduler reports a bad actuator
    // write, the app decides what that means for capabilities and arming.
    PlaybackScheduler::Deps sd;
    sd.instrument = &g_instrument;
    sd.steppers = &g_steppers;
    sd.servos = &g_servos;
    sd.actuators = &g_actuators;
    sd.diag = &g_diag;
    sd.profile = &g_profile;
    sd.axisFaulted = &g_axisFaulted;
    sd.fault = [](size_t i, const char* reason, uint32_t nowMs) {
        faultRuntimeAxis(i, reason, nowMs);
    };
    sd.actOk = [](size_t axis, ActuatorResult r, const char* what, uint32_t nowMs) {
        return actOk(axis, r, what, nowMs);
    };
    g_scheduler.bind(sd);

    SafetySupervisor::Deps yd;
    yd.safety = &g_safety;
    yd.steppers = &g_steppers;
    yd.servos = &g_servos;
    yd.instrument = &g_instrument;
    yd.scheduler = &g_scheduler;
    yd.diag = &g_diag;
    yd.profile = &g_profile;
    yd.phase = &g_phase;
    yd.degraded = &g_degraded;
    yd.homingStarted = &g_homingStarted;
    yd.homing = &g_homing;
    yd.anchored = &g_anchored;
    yd.axisFaulted = &g_axisFaulted;
    yd.estopPin = &g_estopPin;
    yd.rebuildCaps = []() { return rebuildRuntimeCapabilities(); };
    yd.notifyCaps = []() { notifyCapabilitiesChanged(); };
    yd.hardStopCleanup = []() {
        g_testOffs.clear();  // drop scheduled test Note Offs so a stale one can't stop
                             // a future note with the same channel/number (audit P1-4)
        // A panic / E-stop supersedes any deferred profile activation: report the
        // waiting command as CANCELLED so a client polling it stops immediately
        // instead of waiting out a "queued" ghost.
        if (uint32_t cid = g_activation.commandId())
            setCommandResult(cid, CommandResultRing::Cancelled);
        g_activation.cancel();
    };
    yd.onReady = []() {
        if (uint32_t cid = g_pendingActivationCmd) {
            setCommandResult(cid, CommandResultRing::Succeeded);
            g_pendingActivationCmd = 0;
        }
    };
    g_supervisor.bind(yd);

    g_storage.begin();
    if (g_storage.degraded()) {
        // A previously-initialised filesystem that won't mount: don't auto-wipe.
        g_safety.recordFault("storage",
            "LittleFS unmountable — profiles unavailable; POST /api/storage/format "
            "to reformat", millis());
    }
    // Never configure GPIO (STEP/DIR/HOME/LIMIT/ENABLE/I²C/PCA/LEDC) from a profile
    // that fails semantic validation — and never FABRICATE one either (audit P0.1).
    // The firmware used to synthesise a Ukulele profile here and arm it: on a real
    // machine that drives someone else's pins at someone else's speeds. With no
    // valid stored profile the runtime now keeps an EMPTY profile (no axes, no
    // servos, no actuator pins) and latches CONFIG_SAFE: network + web come up so a
    // profile can be built or loaded, but no actuator can ever move. The Ukulele
    // template still exists in the web UI — it is just never applied behind the
    // user's back.
    //
    // WHAT boots, and in what order (audit P0/P1 — the persistence model):
    //   1. /current.json — the instrument that was last published, i.e. what was
    //      actually running. This is the answer to "why did my machine come back
    //      different?": it did not, because what runs is what boots.
    //   2. otherwise the startup slot, for installs predating /current.json. They
    //      pick up the new files on their first publish (a lazy migration).
    //   3. then /device.json OVERLAYS the device half. The board, pins, E-stop
    //      wiring and fitted hardware belong to THIS MACHINE, so they must not be
    //      whatever a stored instrument happened to be saved with. This is the same
    //      rule the hot path already applied when loading a slot; it now holds at
    //      boot too, which is where it used to silently not.
    bool haveProfile = (g_storage.loadCurrent(g_profile) ||
                        g_storage.load(g_storage.startupSlot(), g_profile));
    g_storage.loadDevice(g_profile);  // this machine's own config wins, if stored
    haveProfile = haveProfile && ProfileValidator::isActivatable(g_profile);
    if (!haveProfile) {
        g_profile = Profile{};  // empty: nothing to drive
        g_safety.configSafe();
        g_phase = AppPhase::ConfigSafe;
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

    // Wi-Fi secrets live in NVS, never in the exportable profile (§20). The link
    // config lives there too and wins over whatever the profile carries: it
    // describes this machine, so swapping instruments must not move the device to
    // another network (P1.13).
    loadNetworkOverrides(g_profile.network);
    Preferences prefs;
    prefs.begin("gmb", true);
    String staPass = prefs.getString("wifipass", "");
    String apPass = prefs.getString("appass", "");
    prefs.end();
    g_net.begin(g_profile.network, staPass.c_str(), apPass.c_str());
    g_midi.begin(kMidiUdpPort);
    // Restore the stored UDP MIDI source posture (device state, not part of an
    // instrument profile): 0 open (default) / 1 lockToFirst / 2 disabled.
    {
        Preferences p;
        p.begin("gmb", true);
        int midiSrc = p.getInt("midisrc", 0);
        p.end();
        if (midiSrc >= 0 && midiSrc <= 2)
            g_midi.setSourcePolicy(static_cast<UdpSourcePolicy>(midiSrc));
    }
    // Native USB-MIDI: real in the esp32-s3-usbmidi build, inert everywhere else.
    if (g_usbMidi.begin())
        Serial.println("[midi] native USB-MIDI endpoint up");
    bindDinMidi();      // DIN-5/TRS MIDI in, when a MIDI_RX pin is assigned

    WebContext ctx = buildWebContext();
    g_web.begin(ctx, 80);

    // Home every axis before allowing play; unhomed axes never move for notes.
    // beginHoming() itself refuses if the profile is invalid or a channel failed to
    // attach, leaving the system safely in Boot. In CONFIG_SAFE there is nothing to
    // home and nothing may move: the web/network stay up and wait for a profile.
    if (haveProfile) {
        beginHoming(millis());
    } else {
        g_safety.recordFault("boot",
            "no valid startup profile — CONFIG_SAFE: actuators locked out until one "
            "is loaded", millis());
    }
    refreshDiagnosticsJson();  // seed the diagnostics snapshot before the first GET
    g_web.refreshStatus();     // seed the cached status before the first GET
}

void loop() {
    uint32_t nowUs = micros();
    uint32_t nowMs = millis();

    // ---- SAFETY FIRST, literally -------------------------------------------
    //
    // This block used to sit AFTER g_net.tick() and four other service calls. They
    // are all written to be non-blocking, so this was not a live defect — but it
    // made the E-stop's software response time depend on the network stack for no
    // reason at all, and a comment saying "SAFETY FIRST" under five other calls is
    // a comment that will eventually be believed instead of read. Nothing below
    // needs anything above it, so there is no cost to being first.
    //
    // (Still a SOFTWARE safety, and the second line of defence. The hardware E-stop
    // must cut ENABLE and power on its own — see hardware/POWER_AND_SAFETY.md.)
    //
    // 1) Hardware E-stop. Must assert IMMEDIATELY: trip on the raw press this
    //    instant (a false trip only fails safe); the debounced level only filters
    //    the RELEASE so contact bounce can't un-latch it.
    if (g_estopPin >= 0) {
        bool level = digitalRead(g_estopPin) == HIGH;
        bool rawStop = estopAsserted(level);  // same predicate as the pre-arm checks
        bool debouncedStop = estopAsserted(g_estopDeb.update(nowMs, level));
        if ((rawStop || debouncedStop) &&
            g_safety.state() != SafetyState::EmergencyStop) {
            doEmergencyStop();
            purgeCommands();  // drop anything queued behind the E-stop
        }
    }
    // 2) Web/CC STOP flag: honoured before draining, and it purges the queue.
    bool panicked = servicePanic(nowMs);
    // 3) Endstop debouncing, so the per-axis LIMIT/HOME scan below reads settled
    //    levels. Kept with the safety block rather than with the mechanics: it is
    //    what makes a LIMIT trip visible this tick.
    g_steppers.updateSensors(nowMs);

    // ---- everything that is not a safety input ------------------------------
    g_net.tick(nowMs);
    // MIDI source posture changes from POST /api/midi/source. loop() owns the
    // transport, so the web task only leaves a request here.
    {
        int midiPolicy = g_midiSourceRequested.exchange(-1);
        if (midiPolicy >= 0 && midiPolicy <= 2)
            g_midi.setSourcePolicy(static_cast<UdpSourcePolicy>(midiPolicy));
        if (g_midiUnlockRequested.exchange(false)) g_midi.unlockSource();
    }
    serviceHotspotRequests(nowMs);  // BOOT long-press / web "Start hotspot"
    serviceNetworkApply();          // POST /api/wifi with apply:true
    serviceWifiRequests();          // network survey for the Settings picker

    // Apply a BOUNDED number of queued web commands (skip if we just stopped, so
    // no stale activation/test runs after a STOP).
    if (!panicked) drainCommands(nowMs);
    servicePendingActivation(nowMs);  // phase 2 of a deferred profile activation

    // Wi-Fi loss policy (spec §21.4, default): cancel pending
    // commands and release notes in a controlled way, but stay armed/READY.
    static bool wasConnected = false;
    bool nowConnected = g_net.connected() && !g_net.accessPointActive();
    if (!wasConnected && nowConnected) g_diag.addWifiReconnect();
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
    // Ingest MIDI from every transport into the SAME InstrumentController (P1.7).
    // Each event already carries its transport as MidiEvent.source.
    for (MidiTransport* t : g_transports) t->poll(nowUs);
    for (size_t ti = 0; ti < sizeof(g_transports) / sizeof(g_transports[0]); ++ti) {
        MidiTransport* t = g_transports[ti];
        const uint32_t n = static_cast<uint32_t>(t->events().size());
        g_diag.addMidiEvents(n);
        if (n) {
            g_transportEvents[ti].fetch_add(n);
            g_transportLastMs[ti].store(nowMs);
            g_lastTransport.store(static_cast<int>(ti));
        }
        for (auto& e : t->events()) {
            g_web.broadcastMidi(e);  // feed the Web MIDI monitor (all phases)
            if (g_phase == AppPhase::Ready) g_instrument.handleEvent(e, nowUs);
        }
    }
    // SysEx carries an IP-addressed reply, so it stays on the concrete UDP transport
    // (USB/DIN replies will be added on their own transports as they land).
    for (auto& sx : g_midi.sysexPackets()) {
        // Serialise with the Web /api/sysex/request route (shared rate limiter +
        // snapshot); both take g_stateMutex.
        std::vector<uint8_t> resp;
        { StateGuard lock;
          resp = g_sysex.handleMessage(sx.bytes.data(), sx.bytes.size(), nowMs); }
        if (!resp.empty()) g_midi.reply(sx, resp.data(), resp.size());
    }
    for (MidiTransport* t : g_transports) t->clear();

    g_instrument.tick(nowUs);   // flush chord groups
    g_servos.update(nowMs);     // scheduled servo returns / rest cut-off

    // Runtime PCA9685 health: a board lost AFTER arming (unplugged / brown-out)
    // means no finger/pluck can act — panic rather than keep "playing" blind.
    static uint32_t lastPcaCheckMs = 0;
    if (g_phase == AppPhase::Ready && nowMs - lastPcaCheckMs >= 500) {
        lastPcaCheckMs = nowMs;
        uint8_t failedBoard = 0xFF;
        bool healthy = g_servos.pcaHealthy(failedBoard);
        // Cache the result for the web task: /api/diagnostics must never touch I2C.
        g_diag.setPca(g_servos.usesPca(), healthy, healthy ? 0xFF : failedBoard);
        if (!healthy) serviceLostPcaBoard(failedBoard, nowMs);
    }

    if (g_phase == AppPhase::Homing) {
        doHoming(nowMs);
        g_steppers.tick(nowUs);
    } else if (g_phase == AppPhase::Ready && g_safety.actuatorsAllowed()) {
        // Endstop safety for EVERY axis first (active or not), then the musical
        // logic for the axes that did not just fault.
        for (size_t i = 0; i < g_instrument.stringCount(); ++i) {
            if (!g_scheduler.tickAxisSafety(i, nowMs)) g_scheduler.tick(i, nowMs);
        }
        g_steppers.tick(nowUs);
    }

    // Scheduler health: the period of this loop() is the latency budget every
    // musical deadline lives inside, so its worst case and jitter are tracked
    // (audit P2.19). Seeded on the first tick, then fed every tick.
    {
        static uint32_t lastTickUs = 0;
        if (lastTickUs != 0) g_diag.observeSchedulerPeriodUs(nowUs - lastTickUs);
        lastTickUs = nowUs;
    }

    static uint32_t lastStatusMs = 0;
    if (nowMs - lastStatusMs >= 100) {
        lastStatusMs = nowMs;
        // Refresh the counters the components already total up, so the web task
        // reads a plain snapshot and never walks live runtime structures.
        g_diag.setFaults(g_safety.faultCount());
        g_diag.setServoMoves(g_servos.moveCount());
        g_diag.setAxisMoves(g_steppers.moveCount());
        g_diag.setMidiDropped(g_midi.droppedEvents());
        g_diag.setUdpDropped(g_midi.droppedPackets());
        g_diag.setUdpRejected(g_midi.rejectedPackets());
        g_diag.setGovernorThrottles(g_actuators.throttleCount());
        g_diag.setMoveMix(g_actuators.deadlineMoves(), g_actuators.staggerableGrants());
        refreshDiagnosticsJson();  // publish the snapshot GET /api/diagnostics serves
        g_web.refreshStatus();     // rebuild the cached snapshot from live state...
        g_web.broadcastStatus();   // ...then push it (GET /api/status serves it too)
    }
}

#endif  // ARDUINO
