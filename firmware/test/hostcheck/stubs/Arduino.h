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

// ---- controllable pin levels and clock ------------------------------------
//
// digitalRead() used to be a constant HIGH and millis() a constant 0, which is
// enough to COMPILE the platform layer but makes whole behaviours untestable: a
// homing sequence is "watch a sensor change while time passes", and with a frozen
// sensor and a frozen clock there is nothing to watch. Both now read from a table
// a harness can drive, defaulting to exactly the old values so every existing
// harness behaves identically unless it opts in.
inline int* gmbPinLevels() {
    static int levels[64];
    static bool init = false;
    if (!init) { for (int& v : levels) v = HIGH; init = true; }
    return levels;
}
inline void gmbSetPinLevel(int pin, int level) {
    if (pin >= 0 && pin < 64) gmbPinLevels()[pin] = level;
}
inline unsigned long& gmbClockMs() { static unsigned long ms = 0; return ms; }
inline void gmbSetMillis(unsigned long ms) { gmbClockMs() = ms; }
inline void gmbAdvanceMs(unsigned long d) { gmbClockMs() += d; }

inline void pinMode(int, int) {}
inline void digitalWrite(int pin, int level) { gmbSetPinLevel(pin, level); }
inline int digitalRead(int pin) {
    return (pin >= 0 && pin < 64) ? gmbPinLevels()[pin] : HIGH;
}
inline unsigned long millis() { return gmbClockMs(); }
inline unsigned long micros() { return gmbClockMs() * 1000UL; }
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
[[maybe_unused]] static SerialStub Serial;

// Extra UARTs. The DIN-MIDI input binds one of these at 31250 baud with an
// explicit RX pin and no TX, so the stub must accept that exact signature —
// otherwise the host check cannot see a typo on the one line that matters.
static const uint32_t SERIAL_8N1 = 0x800001c;
class HardwareSerial : public Stream {
public:
  explicit HardwareSerial(int uartNr) : uart_(uartNr) {}
  void begin(unsigned long baud, uint32_t config = SERIAL_8N1, int8_t rxPin = -1,
             int8_t txPin = -1) {
    (void)baud; (void)config; (void)rxPin; (void)txPin;
  }
  void end() {}
private:
  int uart_ = 0;
};

// Minimal ESP object (chip identity + heap telemetry for diagnostics).
struct EspClass {
  uint64_t getEfuseMac() { return 0; }
  uint32_t getFreeHeap() { return 0; }
  uint32_t getMinFreeHeap() { return 0; }
};
[[maybe_unused]] static EspClass ESP;
