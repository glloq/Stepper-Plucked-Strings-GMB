#pragma once
// Instrumented PCA9685 driver: every write is logged with the I2C bus it went out
// on (via the TwoWire it was constructed with), its address and channel — so a test
// can prove a bus-1 servo really drives Wire1 and not Wire.
#include <cstdint>
#include <string>
#include <vector>

#include "Wire.h"

struct PwmWrite {
  std::string bus;
  uint8_t addr;
  uint8_t channel;
  uint16_t us;
  bool off;
};
inline std::vector<PwmWrite> g_pwmLog;

class Adafruit_PWMServoDriver {
  uint8_t addr_;
  TwoWire* wire_;

 public:
  Adafruit_PWMServoDriver(uint8_t a = 0x40) : addr_(a), wire_(&Wire) {}
  Adafruit_PWMServoDriver(uint8_t a, TwoWire& w) : addr_(a), wire_(&w) {}
  bool begin(uint8_t = 0) { return true; }
  void setPWMFreq(float) {}
  void writeMicroseconds(uint8_t ch, uint16_t us) {
    g_pwmLog.push_back({wire_->id, addr_, ch, us, false});
  }
  void setPWM(uint8_t ch, uint16_t, uint16_t) {
    g_pwmLog.push_back({wire_->id, addr_, ch, 0, true});
  }
};
