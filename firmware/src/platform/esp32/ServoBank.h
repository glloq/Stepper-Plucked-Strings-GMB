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

    // Non-blocking motion helpers (honour travelMs / settleMs / disableAtRest):
    //   press  : hold active   (finger down)
    //   release: return to rest (finger up), then optionally cut PWM at rest
    //   strike : pulse active then auto-return to rest (pluck / strum / damper)
    void press(int index);
    void release(int index);
    // intensity 0..1 scales the strike depth between rest and active (velocity).
    void strike(int index, double intensity = 1.0);
    // Advance scheduled returns and rest-time PWM cut-off. Call from loop().
    void update(uint32_t nowMs);

    // Hardware safety: enable/disable all PCA outputs via /OE.
    void outputEnable(bool on);
    void neutraliseAll();

    // Lookup by role + string (-1 for shared roles). Returns -1 if absent.
    int servoIndex(const std::string& function, int stringIndex) const;
    int fingerIndex(int stringIndex) const { return servoIndex("finger", stringIndex); }
    int pluckIndex(int stringIndex) const { return servoIndex("pluck", stringIndex); }
    int strumIndex(int stringIndex) const { return servoIndex("strum", stringIndex); }
    int damperIndex(int stringIndex) const { return servoIndex("damper", stringIndex); }
    int sharedStrumIndex() const { return servoIndex("sharedStrum", -1); }

    // True if any configured direct-GPIO servo failed to attach an LEDC channel.
    bool directAttachFault() const { return directAttachFault_; }

    size_t count() const { return servos_.size(); }
    bool usesPca() const { return pcaUsed_; }
    uint16_t travelMs(int index) const {
        return (index >= 0 && index < (int)servos_.size()) ? servos_[index].travelMs : 0;
    }
    uint16_t settleMs(int index) const {
        return (index >= 0 && index < (int)servos_.size()) ? servos_[index].settleMs : 0;
    }

private:
    enum class Mode : uint8_t { Rest, Active, Striking };
    struct Rt {
        Mode mode = Mode::Rest;
        uint32_t returnAtMs = 0;  // when a strike returns to rest
        uint32_t restAtMs = 0;    // when a resting servo may cut its PWM
        bool pwmOff = false;
    };
    std::vector<Rt> rt_;

    std::vector<ServoConfig> servos_;
    std::vector<int8_t> ledcCh_;  // LEDC channel per direct servo (Arduino 2.x)
    std::vector<bool> attached_;  // direct-servo LEDC attach state
    int8_t oePin_ = -1;
    int directCount_ = 0;         // number of LEDC channels handed out
    bool pcaUsed_ = false;
    bool pcaPresent_[kMaxPca] = {false, false, false, false};
    bool directAttachFault_ = false;
    static constexpr int kMaxDirectServos = 8;  // ESP32-S3 has 8 LEDC channels
#if defined(ARDUINO)
    Adafruit_PWMServoDriver pca_[kMaxPca] = {
        Adafruit_PWMServoDriver(0x40), Adafruit_PWMServoDriver(0x41),
        Adafruit_PWMServoDriver(0x42), Adafruit_PWMServoDriver(0x43)};
#endif
    void writeMicros(int index, uint16_t us);
    void writeOff(int index);
    bool attachDirect(int index);  // (re)attach a direct servo's LEDC channel
};

}  // namespace gmb
