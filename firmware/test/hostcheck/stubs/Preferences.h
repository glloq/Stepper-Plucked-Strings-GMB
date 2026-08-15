#pragma once
#include <Arduino.h>
class Preferences {
public:
  bool begin(const char*, bool = false) { return true; }
  String getString(const char*, const char* d = "") { return String(d); }
  // The real one returns the number of bytes written, and callers check it: a
  // stub that always returned 0 made every write look like a failure, which is as
  // untrue as always returning success and just as likely to hide a bug.
  size_t putString(const char*, const String& v) { return v.length() + 1; }
  uint8_t getUChar(const char*, uint8_t d = 0) { return d; }
  size_t putUChar(const char*, uint8_t) { return sizeof(uint8_t); }
  bool getBool(const char*, bool d = false) { return d; }
  size_t putBool(const char*, bool) { return 0; }
  int32_t getInt(const char*, int32_t d = 0) { return d; }
  size_t putInt(const char*, int32_t) { return 0; }
  bool isKey(const char*) { return false; }
  bool remove(const char*) { return true; }
  void end() {}
};
