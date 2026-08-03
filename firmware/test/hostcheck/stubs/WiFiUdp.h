#pragma once
#include <Arduino.h>
#include <WiFi.h>
class WiFiUDP {
public:
  void begin(uint16_t) {}
  int parsePacket() { return 0; }
  int read(uint8_t*, size_t) { return 0; }
  void flush() {}
  void clear() {}
  IPAddress remoteIP() { return IPAddress(); }
  uint16_t remotePort() { return 0; }
  void beginPacket(IPAddress, uint16_t) {}
  size_t write(const uint8_t*, size_t n) { return n; }
  void endPacket() {}
};
