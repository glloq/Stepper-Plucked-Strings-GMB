#pragma once
#include <cstdint>
class TwoWire {
 public:
  void begin(int, int) {}
  void beginTransmission(uint8_t) {}
  uint8_t endTransmission() { return 0; }
};
static TwoWire Wire;
