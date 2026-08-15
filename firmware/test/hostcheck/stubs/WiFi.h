#pragma once
#include <Arduino.h>
#define WL_CONNECTED 3
#define WIFI_SCAN_RUNNING (-1)
#define WIFI_SCAN_FAILED (-2)
enum WiFiMode { WIFI_OFF, WIFI_STA, WIFI_AP, WIFI_AP_STA };
enum WifiAuthMode { WIFI_AUTH_OPEN = 0, WIFI_AUTH_WPA2_PSK = 3 };
// Carries a real value, like the Arduino type. It used to convert to a constant 0,
// which made every remote host look like the same host — fine for a compile check,
// useless for anything that distinguishes senders by (ip, port), which is the whole
// basis of MIDI origins.
class IPAddress {
 public:
  IPAddress() = default;
  explicit IPAddress(uint32_t v) : v_(v) {}
  String toString() const { return String("0.0.0.0"); }
  operator uint32_t() const { return v_; }  // real Arduino IPAddress has this
 private:
  uint32_t v_ = 0;
};
class WiFiClassStub {
public:
  void mode(int) {}
  bool softAP(const char*) { return true; }
  bool softAP(const char*, const char*) { return true; }
  bool softAPdisconnect(bool = false) { return true; }
  IPAddress softAPIP() { return IPAddress(); }
  void setHostname(const char*) {}
  void begin(const char*, const char*) {}
  void disconnect(bool = false) {}
  int status() { return WL_CONNECTED; }
  IPAddress localIP() { return IPAddress(); }
  // Async scan surface (Net::startScan/pollScan).
  int16_t scanNetworks(bool = false, bool = false) { return 0; }
  int16_t scanComplete() { return 0; }
  void scanDelete() {}
  String SSID(int) { return String(""); }
  int32_t RSSI(int) { return -70; }
  int encryptionType(int) { return WIFI_AUTH_OPEN; }
  int32_t channel(int) { return 1; }
};
static WiFiClassStub WiFi;
