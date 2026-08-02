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

std::vector<HomingController> g_homing;
std::vector<bool> g_anchored;

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
            String idx = String(static_cast<int>(i) + 1);
            if (a.signal == String("STEP") + idx) out[i].step = a.gpio;
            else if (a.signal == String("DIR") + idx) out[i].dir = a.gpio;
            else if (a.signal == String("HOME") + idx) out[i].home = a.gpio;
            else if (a.signal == String("LIMIT") + idx) out[i].limit = a.gpio;
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

void beginHoming(uint32_t nowMs) {
    g_steppers.enableDrivers(true);
    g_servos.outputEnable(true);   // servos hold their rest position (fingers up)
    for (size_t i = 0; i < g_homing.size(); ++i) {
        g_instrument.string(i).setHoming();
        g_homing[i].start(nowMs);
        g_anchored[i] = false;
    }
    g_phase = AppPhase::Homing;
}

void doHoming(uint32_t nowMs) {
    bool allDone = true;
    for (size_t i = 0; i < g_homing.size(); ++i) {
        if (g_homing[i].failed()) {
            g_instrument.string(i).disable();  // fault this axis, keep the others
            g_steppers.stop(i);
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
                int pi = g_servos.pluckIndex(static_cast<int>(i));
                if (pi >= 0) g_servos.strike(pi);  // pulse auto-returns to rest
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

    g_net.begin(g_profile.network, /*stationPassword=*/"");
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
    ctx.onActivateProfile = [](const Profile& p) {
        if (!ProfileValidator::isActivatable(p)) return false;
        g_profile = p;
        g_profile.capabilitiesRevision++;
        applyProfile();
        g_sysex.rebuild(g_profile);
        beginHoming(millis());  // re-home after a mechanical config change (§16)
        return true;
    };
    g_web.begin(ctx, 80);

    // Home every axis before allowing play; unhomed axes never move for notes.
    if (valid) beginHoming(millis());
}

void loop() {
    uint32_t nowUs = micros();
    uint32_t nowMs = millis();

    g_net.tick(nowMs);

    // Ingest Wi-Fi MIDI. SysEx is always answered; notes only play once Ready.
    g_midi.poll(nowUs);
    for (auto& e : g_midi.parser().events()) {
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
