// Stepper-Plucked-Strings-GMB — ESP32-S3 firmware entry point.
//
// Wires the pure-core logic (src/core/) to the ESP32 platform adapters
// (src/platform/esp32/). The core is unit-tested on the host; this file is the
// hardware integration and runs only on device.
#if defined(ARDUINO)

#include <Arduino.h>
#include <ArduinoJson.h>

#include <vector>

#include "core/configuration/Profile.h"
#include "core/configuration/ProfileValidator.h"
#include "core/gmb/GmbSysExService.h"
#include "core/instrument/InstrumentController.h"
#include "core/midi/MidiEvent.h"
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

// Per-string non-blocking playback scheduler.
struct StringSched {
    enum Phase { Idle, MovingToFret, PressingFinger, Settling, Ready } phase = Idle;
    uint32_t phaseStartMs = 0;
    uint32_t commandId = 0;
    bool wantPluck = false;
};
std::vector<StringSched> g_sched;

// Resolve the profile pins into per-axis STEP/DIR/HOME and the ENABLE pin.
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

int8_t pinOf(const char* signal) {
    for (const auto& a : g_profile.pins)
        if (a.signal == signal) return a.gpio;
    return -1;
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
}

void doPanic() {
    g_instrument.panic();
    g_steppers.stopAll();
    g_steppers.enableDrivers(false);
    g_servos.neutraliseAll();
    for (auto& s : g_sched) s = StringSched{};
    g_safety.panic("web/CC panic", millis());
}

// Drive one string's mechanical sequence toward a plucked note.
void tickString(size_t i, uint32_t nowMs) {
    StringController& sc = g_instrument.string(i);
    const StringTarget& tgt = g_instrument.target(i);
    StringSched& sch = g_sched[i];

    if (!tgt.active) {
        sch.phase = StringSched::Idle;
        return;
    }

    if (sch.commandId != tgt.commandId) {
        // New note: start the sequence (finger released -> move).
        sch.commandId = tgt.commandId;
        sch.phase = StringSched::MovingToFret;
        sch.phaseStartMs = nowMs;
        g_steppers.moveToMm(i, tgt.positionMm);
    }

    switch (sch.phase) {
        case StringSched::MovingToFret:
            if (g_steppers.atTarget(i)) {
                sc.motionReached();
                if (sc.openString()) {
                    sch.phase = StringSched::Ready;
                } else {
                    int fi = g_servos.fingerIndex(i);
                    if (fi >= 0) g_servos.toActive(fi);
                    sch.phase = StringSched::PressingFinger;
                    sch.phaseStartMs = nowMs;
                }
            }
            break;
        case StringSched::PressingFinger:
            if (nowMs - sch.phaseStartMs >= 120) {  // finger travel
                sc.fingerPressed();
                sch.phase = StringSched::Settling;
                sch.phaseStartMs = nowMs;
            }
            break;
        case StringSched::Settling:
            if (nowMs - sch.phaseStartMs >= 30) {
                sc.settled();
                sch.phase = StringSched::Ready;
            }
            break;
        case StringSched::Ready:
            // Deferred pluck, guarded by command id (cahier des charges §16).
            if (sc.pluckArmed() && sc.executePluck(tgt.commandId)) {
                int pi = g_servos.pluckIndex(i);
                if (pi >= 0) {
                    g_servos.toActive(pi);  // strike
                }
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
        // Fall back to a sane default (4-string ukulele GCEA).
        g_profile = Profile::makeDefault("Ukulele", 4, {67, 60, 64, 69}, 12);
    }

    bool valid = ProfileValidator::isActivatable(g_profile);
    applyProfile();

    // Wi-Fi password is kept outside the exportable profile (NVS/secrets).
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
    ctx.onPanic = doPanic;
    ctx.onActivateProfile = [](const Profile& p) {
        if (!ProfileValidator::isActivatable(p)) return false;
        g_profile = p;
        g_profile.capabilitiesRevision++;
        applyProfile();
        return true;
    };
    g_web.begin(ctx, 80);

    // Only arm actuators once the config and pins are valid
    // (cahier des charges §10.9 / §21.1). Homing would run here per axis.
    if (g_safety.arm(valid, valid)) {
        g_steppers.enableDrivers(true);
        g_servos.outputEnable(true);
    }
}

void loop() {
    uint32_t nowUs = micros();
    uint32_t nowMs = millis();

    g_net.tick(nowMs);

    // Ingest Wi-Fi MIDI.
    g_midi.poll(nowUs);
    for (auto& e : g_midi.parser().events()) g_instrument.handleEvent(e, nowUs);
    for (auto& msg : g_midi.parser().sysex()) {
        auto resp = g_sysex.handleMessage(msg.data(), msg.size(), nowMs);
        if (!resp.empty()) g_midi.send(resp.data(), resp.size());
    }
    g_midi.parser().clear();

    // Advance per-string mechanics and generate step pulses.
    if (g_safety.actuatorsAllowed()) {
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
