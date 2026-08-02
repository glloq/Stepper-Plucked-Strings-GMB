#include "SafetyManager.h"

namespace gmb {

void SafetyManager::panic(const std::string& cause, uint32_t nowMs) {
    recordFault("panic", cause, nowMs);
    state_ = SafetyState::Panic;
}

void SafetyManager::emergencyStop(uint32_t nowMs) {
    recordFault("estop", "hardware emergency stop asserted", nowMs);
    state_ = SafetyState::EmergencyStop;
}

void SafetyManager::recordFault(const std::string& source, const std::string& msg,
                                uint32_t nowMs) {
    faults_.push_back({source, msg, nowMs});
    // Keep the log bounded (fixed memory on the ESP32).
    if (faults_.size() > 64) faults_.erase(faults_.begin());
}

}  // namespace gmb
