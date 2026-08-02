// Minimal LittleFS / FS stub for host compile-checking.
#pragma once
#include <Arduino.h>
#include <cstddef>

namespace fs {
class File : public Stream {
public:
  operator bool() const { return ok_; }
  void close() {}
  int parseInt() { return 0; }
  using Print::print;
  size_t print(int) { return 0; }
  size_t write(uint8_t) override { return 1; }
  size_t write(const uint8_t*, size_t n) override { return n; }
  int read() override { return -1; }
  size_t readBytes(char*, size_t) override { return 0; }
  bool ok_ = true;
};
class FS {
public:
  File open(const char*, const char* = "r") { return File(); }
  bool exists(const char*) { return true; }
  bool mkdir(const char*) { return true; }
  bool remove(const char*) { return true; }
  bool rename(const char*, const char*) { return true; }
};
}  // namespace fs

using fs::File;

class LittleFSClass : public fs::FS {
public:
  bool begin(bool = false) { return true; }
  void end() {}
};
static LittleFSClass LittleFS;
