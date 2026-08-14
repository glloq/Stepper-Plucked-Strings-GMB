#pragma once
#include <cstdint>
class TwoWire;
class Adafruit_PWMServoDriver {
public:
  Adafruit_PWMServoDriver(uint8_t = 0x40) {}
  Adafruit_PWMServoDriver(uint8_t, TwoWire&) {}   // board bound to a specific I2C bus
  bool begin(uint8_t = 0) { return true; }
  void setPWMFreq(float) {}
  void writeMicroseconds(uint8_t, uint16_t) {}
  void setPWM(uint8_t, uint16_t, uint16_t) {}
};
