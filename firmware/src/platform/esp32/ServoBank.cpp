#include "ServoBank.h"

#if defined(ARDUINO)
#include <Wire.h>
#endif

namespace gmb {

namespace {
#if defined(ARDUINO)
constexpr uint32_t kServoFreqHz = 50;
constexpr uint8_t kLedcResBits = 16;
// Convert a servo pulse width (µs) to an LEDC duty at 50 Hz (20 ms period).
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
    oePin_ = oePin;
    pcaUsed_ = false;
    for (int i = 0; i < kMaxPca; ++i) pcaPresent_[i] = false;

    // Discover which PCA boards are referenced.
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
    // Attach direct-GPIO servos to LEDC PWM channels.
    for (const auto& s : servos_) {
        if (s.enabled && s.source == ServoSource::DirectGpio && s.gpio >= 0) {
            ledcAttach(s.gpio, kServoFreqHz, kLedcResBits);
        }
    }
#else
    (void)sda;
    (void)scl;
#endif
    neutraliseAll();
}

void ServoBank::writeMicros(const ServoConfig& s, uint16_t us) {
    us = clampPulse(s, us);
#if defined(ARDUINO)
    if (s.source == ServoSource::Pca) {
        if (s.pcaBoard < kMaxPca && pcaPresent_[s.pcaBoard])
            pca_[s.pcaBoard].writeMicroseconds(s.channel, us);
    } else if (s.gpio >= 0) {
        ledcWrite(s.gpio, usToDuty(us));
    }
#else
    (void)us;
#endif
}

void ServoBank::toRest(int index) {
    if (index >= 0 && index < (int)servos_.size())
        writeMicros(servos_[index], servos_[index].restUs);
}

void ServoBank::toActive(int index) {
    if (index >= 0 && index < (int)servos_.size())
        writeMicros(servos_[index], servos_[index].activeUs);
}

void ServoBank::toMicros(int index, uint16_t us) {
    if (index >= 0 && index < (int)servos_.size()) writeMicros(servos_[index], us);
}

void ServoBank::outputEnable(bool on) {
#if defined(ARDUINO)
    if (oePin_ >= 0) digitalWrite(oePin_, on ? LOW : HIGH);  // /OE active-low
#else
    (void)on;
#endif
}

void ServoBank::neutraliseAll() {
    for (int i = 0; i < (int)servos_.size(); ++i) toRest(i);
    outputEnable(false);  // hard-cut PCA outputs
}

int ServoBank::servoIndex(const std::string& function, int stringIndex) const {
    for (size_t i = 0; i < servos_.size(); ++i) {
        if (servos_[i].function == function &&
            servos_[i].stringIndex == stringIndex) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace gmb
