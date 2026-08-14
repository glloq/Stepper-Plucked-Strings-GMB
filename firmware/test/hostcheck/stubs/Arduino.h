// Minimal Arduino.h stub for host compile-checking (NOT for running).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT 0
#define INPUT_PULLUP 2

// PROGMEM / flash intrinsics (ArduinoJson uses these when ARDUINO is defined).
#define PROGMEM
typedef const char* PGM_P;
class __FlashStringHelper;
#define F(x) (reinterpret_cast<const __FlashStringHelper*>(x))
inline uint8_t pgm_read_byte(const void* p) { return *reinterpret_cast<const uint8_t*>(p); }
inline uint16_t pgm_read_word(const void* p) { return *reinterpret_cast<const uint16_t*>(p); }
inline uint32_t pgm_read_dword(const void* p) { return *reinterpret_cast<const uint32_t*>(p); }
inline float pgm_read_float(const void* p) { return *reinterpret_cast<const float*>(p); }
inline void* pgm_read_ptr(const void* p) { return *reinterpret_cast<void* const*>(p); }

class Print {
public:
  virtual ~Print() {}
  virtual size_t write(uint8_t) { return 1; }
  virtual size_t write(const uint8_t* b, size_t n) { (void)b; return n; }
  size_t print(const char* s) { size_t n = 0; while (s && *s) { write((uint8_t)*s++); ++n; } return n; }
};
class Printable {
public:
  virtual ~Printable() {}
  virtual size_t printTo(Print&) const = 0;
};
class Stream : public Print {
public:
  virtual int read() { return -1; }
  virtual int available() { return 0; }
  virtual int peek() { return -1; }
  virtual size_t readBytes(char*, size_t) { return 0; }
};

// A tiny String that supports the operations our code (and ArduinoJson) use.
class String {
public:
  String() {}
  String(const char* s) : s_(s ? s : "") {}
  String(const std::string& s) : s_(s) {}
  String(int v) : s_(std::to_string(v)) {}
  const char* c_str() const { return s_.c_str(); }
  size_t length() const { return s_.size(); }
  long toInt() const { return s_.empty() ? 0 : std::stol(s_); }
  bool concat(const char* s) { s_ += (s ? s : ""); return true; }
  bool concat(const String& o) { s_ += o.s_; return true; }
  String operator+(const String& o) const { return String(s_ + o.s_); }
  bool operator==(const String& o) const { return s_ == o.s_; }
  std::string s_;
};
inline String operator+(const char* a, const String& b) { return String(std::string(a) + b.s_); }

inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return HIGH; }
inline unsigned long millis() { return 0; }
inline unsigned long micros() { return 0; }
inline void delay(unsigned long) {}
inline void delayMicroseconds(unsigned long) {}

// LEDC (Arduino-ESP32 3.x pin-based API)
inline bool ledcAttach(int, uint32_t, uint8_t) { return true; }
inline void ledcWrite(int, uint32_t) {}
inline void ledcDetach(int) {}
// 2.x channel-based API (compiled only when ESP_ARDUINO_VERSION_MAJOR < 3)
inline void ledcSetup(int, uint32_t, uint8_t) {}
inline void ledcAttachPin(int, int) {}
inline void ledcDetachPin(int) {}

struct SerialStub {
  void begin(unsigned long) {}
  void println() {}
  template <typename T> void println(T) {}
  template <typename T> void print(T) {}
  template <typename... Args> void printf(const char*, Args...) {}
};
static SerialStub Serial;

// Minimal ESP object (chip identity + heap telemetry for diagnostics).
struct EspClass {
  uint64_t getEfuseMac() { return 0; }
  uint32_t getFreeHeap() { return 0; }
  uint32_t getMinFreeHeap() { return 0; }
};
static EspClass ESP;
