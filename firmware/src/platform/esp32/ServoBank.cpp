#include "ServoBank.h"

#include <cstdio>

#include "../../core/configuration/ServoStroke.h"

#if defined(ARDUINO)
#include <Wire.h>
#endif

namespace gmb {

namespace {
#if defined(ARDUINO)
constexpr uint32_t kServoFreqHz = 50;
// The ESP32-S3 LEDC supports 1..14 bits; 14 bits @ 50 Hz keeps ~1 µs steps.
constexpr uint8_t kLedcResBits = 14;
uint32_t usToDuty(uint16_t us) {
    const uint32_t maxDuty = (1u << kLedcResBits) - 1;
    return static_cast<uint32_t>((static_cast<uint64_t>(us) * maxDuty) / 20000u);
}
#endif
uint16_t clampPulse(const ServoConfig& s, uint16_t us) {
    if (us < s.pulseMinUs) us = s.pulseMinUs;
    if (us > s.pulseMaxUs) us = s.pulseMaxUs;
    return us;
}
}  // namespace

void ServoBank::begin(const std::vector<ServoConfig>& servos, int8_t sda, int8_t scl,
                      int8_t oePin, int8_t sda2, int8_t scl2, int8_t oePin2) {
    servos_ = servos;
    rt_.assign(servos.size(), Rt{});
    ledcCh_.assign(servos.size(), -1);
    attached_.assign(servos.size(), false);
    oePin_ = oePin;
    oePin2_ = oePin2;
    directCount_ = 0;
    pcaUsed_ = false;
    directAttachFault_ = false;
    pcaAttachFault_ = false;
    for (int b = 0; b < 2; ++b)
        for (int i = 0; i < kMaxPca; ++i) pcaPresent_[b][i] = false;

    bool busUsed[2] = {false, false};
    for (const auto& s : servos_) {
        if (s.enabled && s.source == ServoSource::Pca && s.pcaBoard < kMaxPca) {
            int bus = s.i2cBus > 1 ? 1 : s.i2cBus;
            pcaPresent_[bus][s.pcaBoard] = true;
            pcaUsed_ = true;
            busUsed[bus] = true;
        }
    }

#if defined(ARDUINO)
    // /OE lines high (outputs disabled) for a safe boot, on both buses' safety pins.
    for (int8_t oe : {oePin_, oePin2_}) {
        if (oe >= 0) { pinMode(oe, OUTPUT); digitalWrite(oe, HIGH); }
    }
    // Bring up each I2C controller that actually carries a board (Wire = bus 0,
    // Wire1 = bus 1), then initialise every present board's driver on its own bus.
    if (busUsed[0] && sda >= 0 && scl >= 0) Wire.begin(sda, scl);
    if (busUsed[1] && sda2 >= 0 && scl2 >= 0) Wire1.begin(sda2, scl2);
    for (int b = 0; b < 2; ++b) {
        for (int i = 0; i < kMaxPca; ++i) {
            if (!pcaPresent_[b][i]) continue;
            // begin() returns false if the board does not ACK on its I2C bus.
            if (!pca_[b][i].begin()) pcaAttachFault_ = true;
            pca_[b][i].setPWMFreq(kServoFreqHz);
        }
    }
    // Attach direct-GPIO servos to LEDC channels (max 8 on the ESP32-S3).
    for (size_t i = 0; i < servos_.size(); ++i) {
        const ServoConfig& s = servos_[i];
        if (s.enabled && s.source == ServoSource::DirectGpio && s.gpio >= 0)
            attachDirect(static_cast<int>(i));
    }
#else
    (void)sda; (void)scl; (void)sda2; (void)scl2; (void)busUsed;
#endif
    hardStop();
}

bool ServoBank::attachDirect(int index) {
    if (index < 0 || index >= (int)servos_.size()) return false;
    const ServoConfig& s = servos_[index];
    if (s.source != ServoSource::DirectGpio || s.gpio < 0) return false;
    if (attached_[index]) return true;
#if defined(ARDUINO)
    // Reuse a previously-assigned channel, otherwise allocate a new one.
    int ch = ledcCh_[index];
    if (ch < 0) {
        if (directCount_ >= kMaxDirectServos) {
            directAttachFault_ = true;  // out of LEDC channels
            return false;
        }
        ch = directCount_++;
        ledcCh_[index] = static_cast<int8_t>(ch);
    }
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    if (!ledcAttach(s.gpio, kServoFreqHz, kLedcResBits)) {
        directAttachFault_ = true;
        return false;
    }
#else
    ledcSetup(ch, kServoFreqHz, kLedcResBits);
    ledcAttachPin(s.gpio, ch);
#endif
    attached_[index] = true;
    return true;
#else
    attached_[index] = true;
    return true;
#endif
}

ActuatorResult ServoBank::writeMicros(int index, uint16_t us) {
    if (index < 0 || index >= (int)servos_.size()) return ActuatorResult::InvalidIndex;
    const ServoConfig& s = servos_[index];
    if (!s.enabled) return ActuatorResult::Disabled;  // never drive a disabled servo
    us = clampPulse(s, us);
    rt_[index].lastUs = us;  // logical pulse (pre-inversion), for diagnostics
    // Apply inversion by mirroring within the calibrated pulse window.
    if (s.inverted) us = static_cast<uint16_t>(s.pulseMinUs + s.pulseMaxUs - us);
#if defined(ARDUINO)
    if (s.source == ServoSource::Pca) {
        int bus = s.i2cBus > 1 ? 1 : s.i2cBus;
        // Board never initialised (absent at boot) — distinct from a mid-run I2C loss,
        // which pcaHealthy() surfaces separately.
        if (s.pcaBoard >= kMaxPca || !pcaPresent_[bus][s.pcaBoard])
            return ActuatorResult::DriverUnavailable;
        pca_[bus][s.pcaBoard].writeMicroseconds(s.channel, us);
    } else if (s.gpio >= 0) {
        if (!attached_[index] && !attachDirect(index))
            return ActuatorResult::OutputFault;  // LEDC (re)attach failed
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        ledcWrite(s.gpio, usToDuty(us));
#else
        if (ledcCh_[index] < 0) return ActuatorResult::OutputFault;
        ledcWrite(ledcCh_[index], usToDuty(us));
#endif
    } else {
        return ActuatorResult::OutputFault;  // no output pin configured
    }
    rt_[index].pwmOff = false;
#else
    (void)us;
    if (hostWriteResult) {
        ActuatorResult forced = hostWriteResult(index);
        if (forced != ActuatorResult::Ok) return forced;
    }
#endif
    ++moveCount_;  // a servo pulse actually reached an output (diagnostics, P2.19)
    return ActuatorResult::Ok;
}

void ServoBank::writeOff(int index) {
    if (index < 0 || index >= (int)servos_.size()) return;
    const ServoConfig& s = servos_[index];
#if defined(ARDUINO)
    if (s.source == ServoSource::Pca) {
        int bus = s.i2cBus > 1 ? 1 : s.i2cBus;
        if (s.pcaBoard < kMaxPca && pcaPresent_[bus][s.pcaBoard])
            pca_[bus][s.pcaBoard].setPWM(s.channel, 0, 0);  // full off, no pulse
    } else if (s.gpio >= 0) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        ledcWrite(s.gpio, 0);
        ledcDetach(s.gpio);  // truly release the pin (no residual signal)
#else
        if (ledcCh_[index] >= 0) ledcWrite(ledcCh_[index], 0);
        ledcDetachPin(s.gpio);
#endif
        attached_[index] = false;  // must reattach before the next write
    }
    rt_[index].pwmOff = true;
#else
    (void)s;
#endif
}

ActuatorResult ServoBank::toRest(int index) {
    if (index < 0 || index >= (int)servos_.size()) return ActuatorResult::InvalidIndex;
    return writeMicros(index, servos_[index].restUs);
}

ActuatorResult ServoBank::toActive(int index) {
    if (index < 0 || index >= (int)servos_.size()) return ActuatorResult::InvalidIndex;
    return writeMicros(index, servos_[index].activeUs);
}

ActuatorResult ServoBank::toMicros(int index, uint16_t us) { return writeMicros(index, us); }

ActuatorResult ServoBank::press(int index) {
    if (index < 0 || index >= (int)servos_.size()) return ActuatorResult::InvalidIndex;
    ActuatorResult r = writeMicros(index, servos_[index].activeUs);
    rt_[index].mode = Mode::Active;
    return r;  // non-Ok => the servo could not be driven; caller faults the axis
}

ActuatorResult ServoBank::moveTo(int index, uint16_t us) {
    if (index < 0 || index >= (int)servos_.size()) return ActuatorResult::InvalidIndex;
    ActuatorResult r = writeMicros(index, us);  // clamped to the servo's pulse window
    rt_[index].mode = Mode::Active;             // hold: no rest-time PWM cut during a test
    return r;
}

ActuatorResult ServoBank::mute(int index) {
    if (index < 0 || index >= (int)servos_.size()) return ActuatorResult::InvalidIndex;
    uint16_t m = servos_[index].muteUs;
    if (m == 0) return ActuatorResult::Disabled;  // no mute position calibrated
    ActuatorResult r = writeMicros(index, m);     // clamped to the pulse window
    rt_[index].mode = Mode::Active;   // hold against the string; caller releases later
    return r;
}

ActuatorResult ServoBank::release(int index) {
    if (index < 0 || index >= (int)servos_.size()) return ActuatorResult::InvalidIndex;
    ActuatorResult r = writeMicros(index, servos_[index].restUs);
    rt_[index].mode = Mode::Rest;
    rt_[index].restAtMs = 0;
    return r;
}

ActuatorResult ServoBank::strike(int index, double intensity) {
    if (index < 0 || index >= (int)servos_.size()) return ActuatorResult::InvalidIndex;
    const ServoConfig& s = servos_[index];
    // Velocity shapes the strike depth; on an alternate stroke the up-stroke
    // endpoint is used. The maths lives in servoStrikeTargetUs (unit-tested).
    bool upStroke = s.alternateDirection && rt_[index].strokeParity;
    ActuatorResult r = writeMicros(index, servoStrikeTargetUs(s, intensity, upStroke));
    rt_[index].mode = Mode::Striking;
    rt_[index].returnAtMs = 0;
    // Flip the stroke direction for the next strike on this servo.
    if (s.alternateDirection) rt_[index].strokeParity = !rt_[index].strokeParity;
    return r;
}

void ServoBank::update(uint32_t nowMs) {
    for (size_t i = 0; i < servos_.size(); ++i) {
        const ServoConfig& s = servos_[i];
        Rt& r = rt_[i];
        switch (r.mode) {
            case Mode::Striking:
                // Engagement time comes from strikeDurationMs() — the SAME value the
                // playback scheduler waits on, so a lift can never retract while the
                // stroke is still engaged (strokeMs vs travelMs divergence).
                if (r.returnAtMs == 0)
                    r.returnAtMs = nowMs + strikeDurationMs(static_cast<int>(i));
                if ((int32_t)(nowMs - r.returnAtMs) >= 0) {
                    ActuatorResult res = toRest(static_cast<int>(i));
                    r.mode = Mode::Rest;
                    r.restAtMs = nowMs + s.settleMs;
                    // The automatic return is a SCHEDULED write with no caller to
                    // check it: surface a failure to the owner instead of silently
                    // pretending the plectrum came home. State is finalised BEFORE
                    // the callback (which may abort / hard-stop).
                    if (!ok(res) && updateFault_) updateFault_(static_cast<int>(i), res);
                }
                break;
            case Mode::Rest:
                if (r.restAtMs == 0) r.restAtMs = nowMs + s.settleMs;
                if (s.disableAtRest && !r.pwmOff && (int32_t)(nowMs - r.restAtMs) >= 0)
                    writeOff(static_cast<int>(i));
                break;
            case Mode::Active:
                break;
        }
    }
}

void ServoBank::outputEnable(bool on) {
#if defined(ARDUINO)
    // Drive every configured /OE line together (both buses when the /OE is split).
    for (int8_t oe : {oePin_, oePin2_})
        if (oe >= 0) digitalWrite(oe, on ? LOW : HIGH);  // /OE active-low
#else
    (void)on;
#endif
}

bool ServoBank::pcaHealthy() const {
    uint8_t ignored;
    return pcaHealthy(ignored);
}

bool ServoBank::pcaHealthy(uint8_t& failedBoard) const {
#if defined(ARDUINO)
    if (!pcaUsed_) return true;
    for (int b = 0; b < 2; ++b) {
        TwoWire& w = (b == 0) ? Wire : Wire1;
        for (int i = 0; i < kMaxPca; ++i) {
            if (!pcaPresent_[b][i]) continue;
            // The PCA9685 base address is 0x40 + board index (per bus). A board that
            // has been unplugged / lost power no longer ACKs its address.
            w.beginTransmission(static_cast<uint8_t>(0x40 + i));
            if (w.endTransmission() != 0) {
                failedBoard = static_cast<uint8_t>(b * 8 + i);  // matches board()
                return false;
            }
        }
    }
    return true;
#else
    (void)failedBoard;
    return true;
#endif
}

bool ServoBank::stringUsesBoard(int stringIndex, uint8_t boardBucket) const {
    for (size_t i = 0; i < servos_.size(); ++i) {
        const ServoConfig& s = servos_[i];
        if (s.enabled && s.stringIndex == stringIndex &&
            board(static_cast<int>(i)) == boardBucket)
            return true;
    }
    return false;
}

std::string ServoBank::boardName(uint8_t boardBucket) {
    if (boardBucket == 0xFF) return "direct-GPIO";
    int bus = boardBucket / 8;
    int addr = 0x40 + (boardBucket % 8);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "bus %d / 0x%02X", bus, addr);
    return std::string(buf);
}

void ServoBank::hardStop() {
    // Immediate hard cut: PCA via /OE, direct servos by cutting their PWM now
    // (do not wait for settleMs), regardless of disableAtRest. Used for E-stop /
    // panic / major fault and as the final cut of a controlled park. Never gated on
    // a mechanical movement completing (spec P0 §21.2).
    //
    // Strict pass order: every NEUTRALISING action runs before any I2C traffic. A
    // hard stop is most needed precisely when hardware misbehaves, and a wedged or
    // slow I2C bus in a per-servo loop could delay the direct-GPIO cut behind dozens
    // of blocking PCA transactions.
    outputEnable(false);  // pass 1: every PCA output dead at once (GPIO /OE line)
    for (int i = 0; i < (int)servos_.size(); ++i)  // pass 2: direct PWM off — no
        if (servos_[i].source == ServoSource::DirectGpio) writeOff(i);  // I2C yet
    // pass 3 (best effort, everything already neutralised): clear every PCA channel
    // to FULL OFF. Preloading REST here would mean the next outputEnable(true)
    // released a live pulse on EVERY channel simultaneously — an ungoverned
    // all-servo start (audit P0). With the channels off, arming re-enables /OE over
    // silent outputs and then commands REST progressively through the governed park.
    for (int i = 0; i < (int)servos_.size(); ++i)
        if (servos_[i].source != ServoSource::DirectGpio) writeOff(i);
    for (auto& r : rt_) r = Rt{};  // pass 4: runtime state cleared
    parkActive_ = false;           // a park in progress is cancelled with it
    parkQueue_.clear();
    parkNext_ = 0;
}

void ServoBank::neutralizePcaOutputs() {
    // FULL OFF on every enabled PCA channel, so driving /OE low can never release
    // stale latched pulses on all channels at once (audit P0). Cheap: one I2C
    // write per PCA servo; boards that are absent are skipped by writeOff().
    for (int i = 0; i < (int)servos_.size(); ++i)
        if (servos_[i].enabled && servos_[i].source == ServoSource::Pca) writeOff(i);
}

uint32_t ServoBank::beginGovernedPark(uint32_t nowMs, uint8_t maxConcurrent,
                                      uint8_t maxPerBoard, uint16_t staggerMs) {
    parkGov_.configure(maxConcurrent, maxPerBoard, staggerMs);
    parkGov_.reset();
    parkQueue_.clear();
    parkNext_ = 0;
    parkResult_ = ParkResult{};
    parkDoneAtMs_ = nowMs;
    parkActive_ = true;
    std::vector<uint8_t> boards;
    for (int i = 0; i < (int)servos_.size(); ++i) {
        if (!servos_[i].enabled) continue;  // disabled: never driven, never parked
        parkQueue_.push_back(i);
        boards.push_back(board(i));
    }
    // Upper bound for the caller's pre-homing timeout: the governor's own batch
    // forecast (exact simulation of the stagger schedule from clean windows) plus
    // the slowest servo's travel + settle. With staggerMs == 0 the forecast is 0
    // and this equals parkDurationMs() — the historical behaviour.
    return parkGov_.forecastBatchDelayMs(nowMs, boards.data(), boards.size()) +
           parkDurationMs();
}

ServoBank::ParkResult ServoBank::serviceGovernedPark(uint32_t nowMs) {
    if (!parkActive_) return parkResult_;
    while (parkNext_ < parkQueue_.size()) {
        int i = parkQueue_[parkNext_];
        // One start permit per servo, honouring the global + per-board caps and the
        // stagger window; a servo on no PCA board (direct GPIO, bucket 0xFF) is only
        // bounded by the global cap — the same rule the play-time governor applies.
        if (!parkGov_.requestStart(nowMs, board(i))) break;  // retry next tick
        ActuatorResult r = release(i);
        if (r != ActuatorResult::Ok && r != ActuatorResult::Disabled &&
            parkResult_.ok) {
            parkResult_.ok = false;
            parkResult_.failedServo = i;
            parkResult_.reason = r;
        }
        uint32_t settle = nowMs + servos_[i].travelMs + servos_[i].settleMs;
        if (static_cast<int32_t>(settle - parkDoneAtMs_) > 0) parkDoneAtMs_ = settle;
        ++parkNext_;
    }
    return parkResult_;
}

bool ServoBank::governedParkDone(uint32_t nowMs) const {
    return parkActive_ && parkNext_ >= parkQueue_.size() &&
           static_cast<int32_t>(nowMs - parkDoneAtMs_) >= 0;
}

ServoBank::ParkResult ServoBank::moveAllToRest() {
    // Controlled park phase 1: drive every servo to rest with outputs kept live. Each
    // release() writes the rest pulse and lets update()/disableAtRest cut idle PWM in
    // the normal way; the caller waits parkDurationMs() for the travel to finish.
    // The FIRST failed rest command is reported: a park whose write never reached a
    // servo must not let the caller assume the finger lifted — arming / a profile
    // swap has to abort instead of homing a carriage under a pressed finger.
    ParkResult out;
    for (int i = 0; i < (int)servos_.size(); ++i) {
        ActuatorResult r = release(i);
        // Disabled servos are not part of the mechanical park (never driven).
        if (r == ActuatorResult::Ok || r == ActuatorResult::Disabled) continue;
        if (out.ok) { out.ok = false; out.failedServo = i; out.reason = r; }
    }
    return out;
}

uint32_t ServoBank::parkDurationMs() const {
    uint32_t w = 0;
    for (const auto& s : servos_) {
        if (!s.enabled) continue;
        uint32_t t = static_cast<uint32_t>(s.travelMs) + s.settleMs;
        if (t > w) w = t;
    }
    return w;
}

int ServoBank::servoIndex(const std::string& function, int stringIndex) const {
    for (size_t i = 0; i < servos_.size(); ++i) {
        if (servos_[i].enabled && servos_[i].function == function &&
            servos_[i].stringIndex == stringIndex)
            return static_cast<int>(i);
    }
    return -1;
}

}  // namespace gmb
