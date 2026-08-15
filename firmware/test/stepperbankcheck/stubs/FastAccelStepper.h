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
  void runForward() {
    velocity = +1;
    running = true;
    g_stepLog.push_back({"runForward", 0, index});
  }
  void runBackward() {
    velocity = -1;
    running = true;
    g_stepLog.push_back({"runBackward", 0, index});
  }
  void stopMove() {
    velocity = 0;
    running = false;
    g_stepLog.push_back({"stopMove", 0, index});
  }
  void forceStopAndNewPosition(uint32_t p) {
    velocity = 0;
    running = false;
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
  int velocity = 0;   // -1 / 0 / +1 while in a run{Forward,Backward} seek
};

// ---- optional kinematic model ------------------------------------------------
//
// moveTo() still "arrives" immediately, which is all the bank harness ever needed.
// A HOMING sequence is different: it is approach -> sensor -> back off until the
// sensor RELEASES -> slow approach, and none of that converges if a velocity seek
// leaves the position frozen. Without a position that responds, the controller
// backs off, waits forever for a sensor that cannot release, and aborts — which is
// correct behaviour reported for the wrong reason.
//
// So: a harness may call gmbAdvanceSteppers(dtMs) to advance every stepper that is
// in a velocity seek, at its configured speed. Constant velocity, no acceleration.
// This is enough to test SEQUENCING (what happens in what order, and on which
// sensor edge); it is NOT a timing model and no test should read it as one.
// Harnesses that never call it see exactly the previous behaviour.
inline std::vector<FastAccelStepper*>& gmbLiveSteppers() {
  static std::vector<FastAccelStepper*> v;
  return v;
}
inline void gmbAdvanceSteppers(uint32_t dtMs) {
  for (FastAccelStepper* s : gmbLiveSteppers()) {
    if (!s || s->velocity == 0) continue;
    double steps = static_cast<double>(s->speedHz) * dtMs / 1000.0;
    s->position += static_cast<int32_t>(s->velocity * steps);
  }
}

// How many generators the fake engine may hand out. Set it below the number of
// enabled axes to exercise the "no free RMT/MCPWM unit" attach-fault path.
inline int g_maxGenerators = 8;

class FastAccelStepperEngine {
public:
  void init() {
    handedOut_ = 0;
    gmbLiveSteppers().clear();   // a fresh begin() replaces the previous axis set
  }
  FastAccelStepper* stepperConnectToPin(uint8_t) {
    if (handedOut_ >= g_maxGenerators) return nullptr;  // out of hardware units
    FastAccelStepper* s = &pool_[handedOut_];
    *s = FastAccelStepper{};   // no state carried over from a previous bank
    s->index = handedOut_;
    ++handedOut_;
    gmbLiveSteppers().push_back(s);
    return s;
  }
  int handedOut() const { return handedOut_; }

private:
  static constexpr int kPool = 8;
  FastAccelStepper pool_[kPool];
  int handedOut_ = 0;
};
