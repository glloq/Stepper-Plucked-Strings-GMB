#pragma once
#include <Arduino.h>
class Preferences {
public:
  bool begin(const char*, bool = false) { return true; }
  String getString(const char*, const char* d = "") { return String(d); }
  size_t putString(const char*, const String&) { return 0; }
  bool getBool(const char*, bool d = false) { return d; }
  size_t putBool(const char*, bool) { return 0; }
  void end() {}
};
