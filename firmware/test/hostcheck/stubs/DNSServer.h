#pragma once
// Minimal DNSServer stub for the host compile-check (captive portal).
#include <cstdint>
class DNSServer {
public:
  // Templated on the IP type so we don't need the WiFi stub's IPAddress here.
  template <class IP>
  bool start(uint16_t, const char*, const IP&) { return true; }
  void processNextRequest() {}
  void stop() {}
};
