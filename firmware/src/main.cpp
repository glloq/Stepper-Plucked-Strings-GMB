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

std::vector<HomingController> g_homing;
std::vector<bool> g_anchored;

int8_t g_estopPin = -1;

// Pending test-note Note Off (scheduled by /api/test/note).
struct TestNoteOff { bool armed = false; uint8_t channel; uint8_t note; uint32_t atMs; };
TestNoteOff g_testOff;

// Per-string non-blocking playback scheduler.
struct StringSched {
    enum Phase { Idle, MovingToFret, PressingFinger, Settling, Ready } phase = Idle;
    uint32_t phaseStartMs = 0;
    uint32_t commandId = 0;
    int fingerIndex = -1;
};
std::vector<StringSched> g_sched;

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

void applyProfile() {
    g_instrument.load(g_profile);
    g_sysex.rebuild(g_profile);
    g_sched.assign(g_profile.strings.size(), StringSched{});

    std::vector<AxisPins> axisPins;
    int8_t enablePin = -1;
    buildStepperPins(axisPins, enablePin);
    g_steppers.begin(g_profile.strings, axisPins, enablePin);
    g_servos.begin(g_profile.servos, pinOf("SDA"), pinOf("SCL"), pinOf("SERVO_OE"));

    g_homing.assign(g_profile.strings.size(), HomingController{});
    g_anchored.assign(g_profile.strings.size(), false);
    for (size_t i = 0; i < g_profile.strings.size(); ++i) {
        if (i < g_profile.homing.size()) g_homing[i].configure(g_profile.homing[i]);
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
    if (!ProfileValidator::isActivatable(g_profile)) return false;
    if (g_steppers.attachFault() || g_servos.directAttachFault()) {
        g_safety.recordFault("attach", "a motor/servo could not attach a channel",
                             nowMs);
        return false;
    }
    g_degraded = false;
    g_steppers.enableDrivers(true);
    g_servos.outputEnable(true);   // servos hold their rest position (fingers up)
    for (size_t i = 0; i < g_homing.size(); ++i) {
        g_instrument.string(i).setHoming();
        g_homing[i].start(nowMs);
        g_anchored[i] = false;
    }
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
    if (g_steppers.attachFault() || g_servos.directAttachFault()) return false;
    g_safety.reset();                 // Panic/EStop -> PowerOnSafe
    g_safety.clearFaults();
    return beginHoming(nowMs);         // mandatory re-home before playing again
}

void doHoming(uint32_t nowMs) {
    bool allDone = true;
    int faulted = 0;
    for (size_t i = 0; i < g_homing.size(); ++i) {
        if (g_homing[i].failed()) {
            g_instrument.string(i).disable();  // fault this axis, keep the others
            g_steppers.stop(i);
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
        HomingCommand cmd = g_homing[i].update(nowMs, rawHigh, g_steppers.positionMm(i));
        switch (cmd.kind) {
            case MoveKind::Stop: g_steppers.stop(i); break;
            case MoveKind::MoveVelocity: g_steppers.setVelocityMm(i, cmd.velocityMmS); break;
            case MoveKind::MoveTo: g_steppers.moveToMmRaw(i, cmd.targetMm); break;
        }
    }
    if (allDone) {
        g_phase = AppPhase::Ready;
        g_safety.arm(true, true);  // profile already validated before homing
        // Degraded run (cahier des charges §13.2): some axes failed homing.
        // Announce the reduced polyphony so General-Midi-Boop stops sending notes
        // the instrument can no longer play, and bump the capabilities revision.
        int ready = static_cast<int>(g_homing.size()) - faulted;
        g_degraded = faulted > 0;
        if (g_degraded) {
            g_profile.capabilitiesRevision++;
            g_sysex.rebuild(g_profile, ready);  // polyphony override = working axes
            g_safety.recordFault("homing",
                                 std::to_string(faulted) + " axis/axes failed homing "
                                 "(degraded run)", nowMs);
        }
    }
}

void doPanic() {
    g_instrument.panic();
    g_steppers.stopAll();
    g_steppers.enableDrivers(false);
    g_servos.neutraliseAll();
    for (auto& s : g_sched) s = StringSched{};
    g_safety.panic("web/CC panic", millis());
    g_phase = AppPhase::Boot;
}

// Drive one string's mechanical sequence toward a plucked note.
void tickString(size_t i, uint32_t nowMs) {
    StringController& sc = g_instrument.string(i);
    const StringTarget& tgt = g_instrument.target(i);
    StringSched& sch = g_sched[i];

    if (!tgt.active) {
        if (sch.phase != StringSched::Idle) {
            // Note released: lift the finger and pulse the damper.
            int fi = g_servos.fingerIndex(static_cast<int>(i));
            if (fi >= 0) g_servos.release(fi);
            int di = g_servos.damperIndex(static_cast<int>(i));
            if (di >= 0) g_servos.strike(di);
            sc.dampingDone();
            sch.phase = StringSched::Idle;
            sch.commandId = 0;
        }
        return;
    }

    if (sch.commandId != tgt.commandId) {
        // New note: lift the finger BEFORE moving the carriage (cahier §16).
        sch.commandId = tgt.commandId;
        sch.fingerIndex = g_servos.fingerIndex(static_cast<int>(i));
        if (sch.fingerIndex >= 0) g_servos.release(sch.fingerIndex);
        g_steppers.moveToMm(i, tgt.positionMm);
        sch.phase = StringSched::MovingToFret;
        sch.phaseStartMs = nowMs;
    }

    // A LIMIT switch tripped during a move faults this axis without disturbing
    // the others (cahier des charges §13.2).
    if (g_steppers.limitActive(i)) {
        g_steppers.emergencyStop(i);   // immediate hard stop, not a decel
        sc.fault();
        g_anchored[i] = false;         // position invalid: require a re-home
        g_safety.recordFault("limit", "LIMIT tripped on axis " + std::to_string(i),
                             nowMs);
        sch.phase = StringSched::Idle;
        return;
    }

    switch (sch.phase) {
        case StringSched::MovingToFret:
            if (g_steppers.atTarget(i)) {
                sc.motionReached();
                if (sc.openString() || sch.fingerIndex < 0) {
                    sch.phase = StringSched::Ready;  // no finger press for open string
                } else {
                    g_servos.press(sch.fingerIndex);
                    sch.phase = StringSched::PressingFinger;
                    sch.phaseStartMs = nowMs;
                }
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
            }
            break;
        case StringSched::Ready:
            if (sc.pluckArmed() && sc.executePluck(tgt.commandId)) {
                // Individual plectrum if present, otherwise the shared strummer
                // (pluckMode Individual / SharedStrum / Both). Velocity shapes
                // the strike depth.
                int pi = g_servos.pluckIndex(static_cast<int>(i));
                if (pi < 0) pi = g_servos.sharedStrumIndex();
                if (pi >= 0) g_servos.strike(pi, tgt.intensity);
            }
            break;
        case StringSched::Idle:
            break;
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    g_safety.boot();  // drivers off, servos neutralised (cahier des charges §21.1)

    g_storage.begin();
    if (!g_storage.load(g_storage.startupSlot(), g_profile)) {
        g_profile = Profile::makeDefault("Ukulele", 4, {67, 60, 64, 69}, 12);
    }

    bool valid = ProfileValidator::isActivatable(g_profile);
    applyProfile();

    // Optional hardware E-stop input (cahier des charges §21.2).
    g_estopPin = pinOf("ESTOP");
    if (g_estopPin >= 0) pinMode(g_estopPin, INPUT_PULLUP);

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
    ctx.onPanic = doPanic;
    // Test note: only accepted when Ready; schedules the matching Note Off.
    ctx.onTestNote = [](uint8_t channel, uint8_t note, uint8_t vel,
                        uint16_t durationMs) -> bool {
        if (g_phase != AppPhase::Ready) return false;
        MidiEvent on;
        on.type = static_cast<uint8_t>(MidiType::NoteOn);
        on.channel = channel; on.data1 = note; on.data2 = vel;
        on.timestampUs = micros();
        on.source = static_cast<uint8_t>(MidiSource::WebUiTest);
        g_instrument.handleEvent(on, on.timestampUs);
        uint32_t offAt = millis() + (durationMs ? durationMs : 500u);
        g_testOff = {true, channel, note, offAt};
        return true;
    };
    ctx.onSetWifi = [](bool hasSta, const std::string& sta, bool hasAp,
                       const std::string& ap) {
        Preferences p;
        p.begin("gmb", false);
        if (hasSta) p.putString("wifipass", String(sta.c_str()));  // only overwrite
        if (hasAp) p.putString("appass", String(ap.c_str()));      // provided fields
        p.end();
    };
    ctx.appState = []() -> std::string {
        if (g_phase == AppPhase::Ready) return g_degraded ? "readyDegraded" : "ready";
        return g_phase == AppPhase::Homing ? "homing" : "boot";
    };
    ctx.onActivateProfile = [](const Profile& p) {
        if (!ProfileValidator::isActivatable(p)) return false;
        // Refuse to (re)start motion while a panic / E-stop is latched; the user
        // must explicitly reset first (POST /api/reset).
        if (safetyLocked()) return false;
        g_profile = p;
        g_profile.capabilitiesRevision++;
        applyProfile();
        g_sysex.rebuild(g_profile);
        return beginHoming(millis());  // re-home after a mechanical change (§16)
    };
    ctx.onReset = []() { return doReset(millis()); };
    g_web.begin(ctx, 80);

    // Home every axis before allowing play; unhomed axes never move for notes.
    // beginHoming() itself refuses if the profile is invalid or a channel failed
    // to attach, leaving the system safely in Boot.
    beginHoming(millis());
    (void)valid;
}

void loop() {
    uint32_t nowUs = micros();
    uint32_t nowMs = millis();

    g_net.tick(nowMs);

    // Hardware E-stop (active-low) latches a panic immediately.
    if (g_estopPin >= 0 && digitalRead(g_estopPin) == LOW &&
        g_phase != AppPhase::Boot) {
        doPanic();
    }

    // Deliver a scheduled test-note Note Off.
    if (g_testOff.armed && (int32_t)(nowMs - g_testOff.atMs) >= 0) {
        MidiEvent off;
        off.type = static_cast<uint8_t>(MidiType::NoteOff);
        off.channel = g_testOff.channel; off.data1 = g_testOff.note;
        off.timestampUs = nowUs;
        g_instrument.handleEvent(off, nowUs);
        g_testOff.armed = false;
    }

    // Ingest Wi-Fi MIDI. SysEx is always answered; notes only play once Ready.
    g_midi.poll(nowUs);
    for (auto& e : g_midi.parser().events()) {
        g_web.broadcastMidi(e);  // feed the Web MIDI monitor (all phases)
        if (g_phase == AppPhase::Ready) g_instrument.handleEvent(e, nowUs);
    }
    for (auto& msg : g_midi.parser().sysex()) {
        auto resp = g_sysex.handleMessage(msg.data(), msg.size(), nowMs);
        if (!resp.empty()) g_midi.send(resp.data(), resp.size());
    }
    g_midi.parser().clear();

    g_instrument.tick(nowUs);   // flush chord groups
    g_servos.update(nowMs);     // scheduled servo returns / rest cut-off

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
        g_web.broadcastStatus();
    }
}

#endif  // ARDUINO
