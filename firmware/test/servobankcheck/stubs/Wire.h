#pragma once
// Instrumented TwoWire: records begin() and carries an id so the PCA driver stub
// can report WHICH bus a servo write actually went to.
#include <cstdint>
class TwoWire {
 public:
  const char* id;
  int lastSda = -1, lastScl = -1, beginCount = 0;
  explicit TwoWire(const char* n) : id(n) {}
  void begin(int sda, int scl) { lastSda = sda; lastScl = scl; ++beginCount; }
  void beginTransmission(uint8_t) {}
  uint8_t endTransmission() { return 0; }
};
inline TwoWire Wire{"Wire"};
inline TwoWire Wire1{"Wire1"};
