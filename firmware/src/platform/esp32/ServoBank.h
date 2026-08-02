// Servo bank supporting PCA9685 (up to four boards) AND direct-GPIO servos,
// mixable per servo (user requirement: work with or without a PCA). Roles:
// finger / pluck / strum / damper per string, plus shared and aux actuators.
// The PCA /OE line is tied to a safety pin so all PCA servos can be neutralised
// instantly (cahier des charges §21.2); direct servos are detached on stop.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../../core/configuration/Profile.h"

#if defined(ARDUINO)
#include <Adafruit_PWMServoDriver.h>
#include <Arduino.h>
#endif

namespace gmb {

class ServoBank {
public:
    static constexpr int kMaxPca = 4;  // 0x40..0x43

    // sda/scl select the I2C pins; oePin drives /OE (active-low). Any of them may
    // be -1 when unused (e.g. no PCA at all — all servos on direct GPIO).
    void begin(const std::vector<ServoConfig>& servos, int8_t sda, int8_t scl,
               int8_t oePin);

    void toRest(int index);
    void toActive(int index);
    void toMicros(int index, uint16_t us);

    // Hardware safety: enable/disable all PCA outputs via /OE.
    void outputEnable(bool on);
    void neutraliseAll();

    // Lookup by role + string (-1 for shared roles). Returns -1 if absent.
    int servoIndex(const std::string& function, int stringIndex) const;
    int fingerIndex(int stringIndex) const { return servoIndex("finger", stringIndex); }
    int pluckIndex(int stringIndex) const { return servoIndex("pluck", stringIndex); }
    int strumIndex(int stringIndex) const { return servoIndex("strum", stringIndex); }
    int damperIndex(int stringIndex) const { return servoIndex("damper", stringIndex); }

    size_t count() const { return servos_.size(); }
    bool usesPca() const { return pcaUsed_; }

private:
    std::vector<ServoConfig> servos_;
    int8_t oePin_ = -1;
    bool pcaUsed_ = false;
    bool pcaPresent_[kMaxPca] = {false, false, false, false};
#if defined(ARDUINO)
    Adafruit_PWMServoDriver pca_[kMaxPca] = {
        Adafruit_PWMServoDriver(0x40), Adafruit_PWMServoDriver(0x41),
        Adafruit_PWMServoDriver(0x42), Adafruit_PWMServoDriver(0x43)};
#endif
    void writeMicros(const ServoConfig& s, uint16_t us);
};

}  // namespace gmb
