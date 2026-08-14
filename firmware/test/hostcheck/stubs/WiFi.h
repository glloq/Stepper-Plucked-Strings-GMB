#pragma once
#include <Arduino.h>
#define WL_CONNECTED 3
#define WIFI_SCAN_RUNNING (-1)
#define WIFI_SCAN_FAILED (-2)
enum WiFiMode { WIFI_OFF, WIFI_STA, WIFI_AP, WIFI_AP_STA };
enum WifiAuthMode { WIFI_AUTH_OPEN = 0, WIFI_AUTH_WPA2_PSK = 3 };
class IPAddress {
 public:
  String toString() const { return String("0.0.0.0"); }
  operator uint32_t() const { return 0; }  // real Arduino IPAddress has this
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
