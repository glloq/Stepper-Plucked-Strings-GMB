#include "ServoBank.h"

#if defined(ARDUINO)
#include <Wire.h>
#endif

namespace gmb {

void ServoBank::begin(const std::vector<ServoConfig>& servos, int8_t sda, int8_t scl,
                      int8_t oePin) {
    servos_ = servos;
    oePin_ = oePin;
#if defined(ARDUINO)
    if (oePin_ >= 0) {
        pinMode(oePin_, OUTPUT);
        digitalWrite(oePin_, HIGH);  // /OE high = outputs disabled (safe boot)
    }
    Wire.begin(sda, scl);
    pwm_.begin();
    pwm_.setPWMFreq(50);  // standard analog servo frame rate
#else
    (void)sda;
    (void)scl;
#endif
    // Boot safe: everything neutralised (cahier des charges §21.1).
    neutraliseAll();
}

void ServoBank::writeMicros(uint8_t channel, uint16_t us) {
#if defined(ARDUINO)
    pwm_.writeMicroseconds(channel, us);
#else
    (void)channel;
    (void)us;
#endif
}

void ServoBank::toRest(size_t index) {
    if (index < servos_.size()) writeMicros(servos_[index].channel, servos_[index].restUs);
}

void ServoBank::toActive(size_t index) {
    if (index < servos_.size())
        writeMicros(servos_[index].channel, servos_[index].activeUs);
}

void ServoBank::toMicros(size_t index, uint16_t us) {
    if (index >= servos_.size()) return;
    const ServoConfig& s = servos_[index];
    if (us < s.pulseMinUs) us = s.pulseMinUs;
    if (us > s.pulseMaxUs) us = s.pulseMaxUs;
    writeMicros(s.channel, us);
}

void ServoBank::outputEnable(bool on) {
#if defined(ARDUINO)
    if (oePin_ >= 0) digitalWrite(oePin_, on ? LOW : HIGH);  // /OE active-low
#else
    (void)on;
#endif
}

void ServoBank::neutraliseAll() {
    // Send every servo to rest, then assert /OE off for a true hardware cut.
    for (size_t i = 0; i < servos_.size(); ++i) toRest(i);
    outputEnable(false);
}

int ServoBank::fingerIndex(size_t stringIndex) const {
    int seen = -1;
    for (size_t i = 0; i < servos_.size(); ++i) {
        if (servos_[i].function == "finger") {
            if (++seen == static_cast<int>(stringIndex)) return static_cast<int>(i);
        }
    }
    return -1;
}

int ServoBank::pluckIndex(size_t stringIndex) const {
    int seen = -1;
    for (size_t i = 0; i < servos_.size(); ++i) {
        if (servos_[i].function == "pluck") {
            if (++seen == static_cast<int>(stringIndex)) return static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace gmb
