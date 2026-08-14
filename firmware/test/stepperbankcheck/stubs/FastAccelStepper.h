// Instrumented FastAccelStepper stub for stepperbankcheck.
//
// Unlike the hostcheck stub (which only has to COMPILE and hands back a null
// stepper), this one behaves: stepperConnectToPin hands out real objects from a
// bounded pool — so an "out of hardware step generators" case can be forced — and
// every engine call is recorded so the test can assert what actually reached the
// step engine (target, speed, direction, force-stop vs decelerated stop).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct StepLogEntry {
  std::string what;     // "moveTo" | "runForward" | "runBackward" | "stopMove" |
                        // "forceStop" | "setSpeed" | "setPosition"
  int32_t value = 0;    // steps for moveTo/setPosition, Hz for setSpeed
  int index = -1;       // which stepper the call landed on
};
inline std::vector<StepLogEntry> g_stepLog;

class FastAccelStepper {
public:
  int index = -1;
  void setDirectionPin(uint8_t pin, bool countUp = true) {
    dirPin = pin; dirCountUp = countUp;
  }
  void setAutoEnable(bool on) { autoEnable = on; }
  void setSpeedInHz(uint32_t hz) {
    speedHz = hz;
    g_stepLog.push_back({"setSpeed", static_cast<int32_t>(hz), index});
  }
  void setAcceleration(uint32_t a) { accel = a; }
  void moveTo(int32_t steps, bool = false) {
    position = steps;  // the stub "arrives" immediately (no motion model here)
    g_stepLog.push_back({"moveTo", steps, index});
  }
  void runForward() { g_stepLog.push_back({"runForward", 0, index}); }
  void runBackward() { g_stepLog.push_back({"runBackward", 0, index}); }
  void stopMove() { g_stepLog.push_back({"stopMove", 0, index}); }
  void forceStopAndNewPosition(uint32_t p) {
    position = static_cast<int32_t>(p);
    g_stepLog.push_back({"forceStop", static_cast<int32_t>(p), index});
  }
  int32_t getCurrentPosition() { return position; }
  void setCurrentPosition(int32_t p) {
    position = p;
    g_stepLog.push_back({"setPosition", p, index});
  }
  bool isRunning() { return running; }

  int32_t position = 0;
  uint32_t speedHz = 0, accel = 0;
  uint8_t dirPin = 0;
  bool dirCountUp = true, autoEnable = false, running = false;
};

// How many generators the fake engine may hand out. Set it below the number of
// enabled axes to exercise the "no free RMT/MCPWM unit" attach-fault path.
inline int g_maxGenerators = 8;

class FastAccelStepperEngine {
public:
  void init() { handedOut_ = 0; }
  FastAccelStepper* stepperConnectToPin(uint8_t) {
    if (handedOut_ >= g_maxGenerators) return nullptr;  // out of hardware units
    FastAccelStepper* s = &pool_[handedOut_];
    s->index = handedOut_;
    ++handedOut_;
    return s;
  }
  int handedOut() const { return handedOut_; }

private:
  static constexpr int kPool = 8;
  FastAccelStepper pool_[kPool];
  int handedOut_ = 0;
};
