#include "ServoBank.h"

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
                      int8_t oePin) {
    servos_ = servos;
    rt_.assign(servos.size(), Rt{});
    ledcCh_.assign(servos.size(), -1);
    oePin_ = oePin;
    pcaUsed_ = false;
    directAttachFault_ = false;
    for (int i = 0; i < kMaxPca; ++i) pcaPresent_[i] = false;

    for (const auto& s : servos_) {
        if (s.enabled && s.source == ServoSource::Pca && s.pcaBoard < kMaxPca) {
            pcaPresent_[s.pcaBoard] = true;
            pcaUsed_ = true;
        }
    }

#if defined(ARDUINO)
    if (oePin_ >= 0) {
        pinMode(oePin_, OUTPUT);
        digitalWrite(oePin_, HIGH);  // /OE high = PCA outputs disabled (safe boot)
    }
    if (pcaUsed_ && sda >= 0 && scl >= 0) {
        Wire.begin(sda, scl);
        for (int i = 0; i < kMaxPca; ++i) {
            if (!pcaPresent_[i]) continue;
            pca_[i].begin();
            pca_[i].setPWMFreq(kServoFreqHz);
        }
    }
    // Attach direct-GPIO servos to LEDC channels (max 8 on the ESP32-S3).
    int direct = 0;
    for (size_t i = 0; i < servos_.size(); ++i) {
        const ServoConfig& s = servos_[i];
        if (!(s.enabled && s.source == ServoSource::DirectGpio && s.gpio >= 0)) continue;
        if (direct >= kMaxDirectServos) {
            directAttachFault_ = true;  // out of LEDC channels
            continue;
        }
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        if (!ledcAttach(s.gpio, kServoFreqHz, kLedcResBits)) {
            directAttachFault_ = true;
            continue;
        }
#else
        ledcSetup(direct, kServoFreqHz, kLedcResBits);
        ledcAttachPin(s.gpio, direct);
#endif
        ledcCh_[i] = static_cast<int8_t>(direct);
        ++direct;
    }
#else
    (void)sda;
    (void)scl;
#endif
    neutraliseAll();
}

void ServoBank::writeMicros(int index, uint16_t us) {
    if (index < 0 || index >= (int)servos_.size()) return;
    const ServoConfig& s = servos_[index];
    us = clampPulse(s, us);
    // Apply inversion by mirroring within the calibrated pulse window.
    if (s.inverted) us = static_cast<uint16_t>(s.pulseMinUs + s.pulseMaxUs - us);
#if defined(ARDUINO)
    if (s.source == ServoSource::Pca) {
        if (s.pcaBoard < kMaxPca && pcaPresent_[s.pcaBoard])
            pca_[s.pcaBoard].writeMicroseconds(s.channel, us);
    } else if (s.gpio >= 0) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        ledcWrite(s.gpio, usToDuty(us));
#else
        if (ledcCh_[index] >= 0) ledcWrite(ledcCh_[index], usToDuty(us));
#endif
    }
    rt_[index].pwmOff = false;
#else
    (void)us;
#endif
}

void ServoBank::writeOff(int index) {
    if (index < 0 || index >= (int)servos_.size()) return;
    const ServoConfig& s = servos_[index];
#if defined(ARDUINO)
    if (s.source == ServoSource::Pca) {
        if (s.pcaBoard < kMaxPca && pcaPresent_[s.pcaBoard])
            pca_[s.pcaBoard].setPWM(s.channel, 0, 0);  // full off, no pulse
    } else if (s.gpio >= 0) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        ledcWrite(s.gpio, 0);
        ledcDetach(s.gpio);  // truly release the pin (no residual signal)
#else
        if (ledcCh_[index] >= 0) ledcWrite(ledcCh_[index], 0);
        ledcDetachPin(s.gpio);
#endif
    }
    rt_[index].pwmOff = true;
#else
    (void)s;
#endif
}

void ServoBank::toRest(int index) {
    if (index >= 0 && index < (int)servos_.size()) writeMicros(index, servos_[index].restUs);
}

void ServoBank::toActive(int index) {
    if (index >= 0 && index < (int)servos_.size())
        writeMicros(index, servos_[index].activeUs);
}

void ServoBank::toMicros(int index, uint16_t us) { writeMicros(index, us); }

void ServoBank::press(int index) {
    if (index < 0 || index >= (int)servos_.size()) return;
    toActive(index);
    rt_[index].mode = Mode::Active;
}

void ServoBank::release(int index) {
    if (index < 0 || index >= (int)servos_.size()) return;
    toRest(index);
    rt_[index].mode = Mode::Rest;
    rt_[index].restAtMs = 0;
}

void ServoBank::strike(int index, double intensity) {
    if (index < 0 || index >= (int)servos_.size()) return;
    const ServoConfig& s = servos_[index];
    if (intensity < 0.0) intensity = 0.0;
    if (intensity > 1.0) intensity = 1.0;
    // Velocity shapes the strike depth between rest and active.
    int span = static_cast<int>(s.activeUs) - static_cast<int>(s.restUs);
    uint16_t us = static_cast<uint16_t>(s.restUs + intensity * span);
    writeMicros(index, us);
    rt_[index].mode = Mode::Striking;
    rt_[index].returnAtMs = 0;
}

void ServoBank::update(uint32_t nowMs) {
    for (size_t i = 0; i < servos_.size(); ++i) {
        const ServoConfig& s = servos_[i];
        Rt& r = rt_[i];
        switch (r.mode) {
            case Mode::Striking:
                if (r.returnAtMs == 0) r.returnAtMs = nowMs + s.travelMs;
                if ((int32_t)(nowMs - r.returnAtMs) >= 0) {
                    toRest(static_cast<int>(i));
                    r.mode = Mode::Rest;
                    r.restAtMs = nowMs + s.settleMs;
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
    if (oePin_ >= 0) digitalWrite(oePin_, on ? LOW : HIGH);  // /OE active-low
#else
    (void)on;
#endif
}

void ServoBank::neutraliseAll() {
    // Immediate hard cut: PCA via /OE, direct servos by cutting their PWM now
    // (do not wait for settleMs), regardless of disableAtRest.
    outputEnable(false);
    for (int i = 0; i < (int)servos_.size(); ++i) {
        if (servos_[i].source == ServoSource::DirectGpio)
            writeOff(i);
        else
            toRest(i);
        if (i < (int)rt_.size()) rt_[i] = Rt{};
    }
}

int ServoBank::servoIndex(const std::string& function, int stringIndex) const {
    for (size_t i = 0; i < servos_.size(); ++i) {
        if (servos_[i].function == function && servos_[i].stringIndex == stringIndex)
            return static_cast<int>(i);
    }
    return -1;
}

}  // namespace gmb
