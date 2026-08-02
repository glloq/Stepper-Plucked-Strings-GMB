// PCA9685 servo bank: finger press, individual pluck, dampers/aux
// (cahier des charges §7.3, §15). The /OE line is tied to a safety pin so all
// servos can be neutralised instantly (cahier des charges §21.2).
#pragma once

#include <cstdint>
#include <vector>

#include "../../core/configuration/Profile.h"

#if defined(ARDUINO)
#include <Adafruit_PWMServoDriver.h>
#include <Arduino.h>
#endif

namespace gmb {

class ServoBank {
public:
    // sda/scl select the I2C pins; oePin drives /OE (active-low output enable).
    void begin(const std::vector<ServoConfig>& servos, int8_t sda, int8_t scl,
               int8_t oePin);

    // Move a logical servo (by index into the configured list) to its rest or
    // active position, or to an explicit microsecond pulse.
    void toRest(size_t index);
    void toActive(size_t index);
    void toMicros(size_t index, uint16_t us);

    // Hardware safety: enable/disable ALL outputs via /OE.
    void outputEnable(bool on);
    void neutraliseAll();

    // Convenience lookups by function + string index (finger/pluck per string).
    int fingerIndex(size_t stringIndex) const;
    int pluckIndex(size_t stringIndex) const;

    size_t count() const { return servos_.size(); }

private:
    std::vector<ServoConfig> servos_;
    int8_t oePin_ = -1;
#if defined(ARDUINO)
    Adafruit_PWMServoDriver pwm_{0x40};
#endif
    void writeMicros(uint8_t channel, uint16_t us);
};

}  // namespace gmb
