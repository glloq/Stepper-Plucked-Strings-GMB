#pragma once
#include <cstdint>
class FastAccelStepper {
public:
  void setDirectionPin(uint8_t, bool = true) {}
  void setAutoEnable(bool) {}
  void setSpeedInHz(uint32_t) {}
  void setAcceleration(uint32_t) {}
  void moveTo(int32_t, bool = false) {}
  void runForward() {}
  void runBackward() {}
  void stopMove() {}
  void forceStopAndNewPosition(uint32_t) {}
  int32_t getCurrentPosition() { return 0; }
  void setCurrentPosition(int32_t) {}
  bool isRunning() { return false; }
};
class FastAccelStepperEngine {
public:
  void init() {}
  FastAccelStepper* stepperConnectToPin(uint8_t) { return nullptr; }
};
