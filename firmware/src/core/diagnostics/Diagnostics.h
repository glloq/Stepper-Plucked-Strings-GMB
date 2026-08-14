// Runtime telemetry accumulator (audit P2.19).
//
// Collects the metrics a future physical bench needs — event/drop counters,
// command-queue high-water, scheduler latency & jitter, fault/movement counters —
// in pure C++17 so the accumulation logic is unit-tested on the host. The
// hardware-specific readings (uptime, reset reason, free heap, per-board PCA
// status) are added by the platform layer at serialisation time; this class owns
// only the values the runtime must track over time.
//
// Stepper build: on top of the servo counters this also tracks the motion-side
// numbers that only exist here — carriage moves issued, homing failures, LIMIT
// trips and move-watchdog timeouts — so a bench run can tell a mechanical problem
// (an axis that keeps missing HOME) from an electrical one (a PCA that dropped).
#pragma once

#include <cstdint>

namespace gmb {

struct DiagCounters {
    uint32_t midiEvents = 0;         // channel-voice events ingested
    uint32_t midiDropped = 0;        // per-tick event overflow (from the transport)
    uint32_t udpDropped = 0;         // oversized / dropped datagrams (from the transport)
    uint32_t udpRejected = 0;        // datagrams refused by the UDP source gate
    uint32_t faults = 0;             // SafetyManager fault records
    uint32_t servoMoves = 0;         // servo pulses actually written
    uint32_t axisMoves = 0;          // carriage moveTo commands issued (steppers)
    uint32_t homingFailures = 0;     // axes that failed to home (cumulative)
    uint32_t limitTrips = 0;         // LIMIT endstop trips that faulted an axis
    uint32_t moveTimeouts = 0;       // moves that missed their watchdog deadline
    uint32_t governorThrottles = 0;  // staggerable start requests the governor deferred
    uint32_t deadlineMoves = 0;      // sound strikes registered (never throttled) — P1.6
    uint32_t staggerableGrants = 0;  // positioning starts the governor granted — P1.6
    uint32_t wifiReconnects = 0;     // station link re-established
    uint32_t cmdQueueHighWater = 0;  // deepest the web->loop command queue ever got
    uint32_t schedulerMaxLatencyUs = 0;  // worst loop() period observed
    uint32_t schedulerJitterUs = 0;      // worst deviation of the period from its mean
    uint32_t schedulerMeanUs = 0;        // smoothed loop() period
    // PCA9685 health, cached from the loop-side probe so the web task never touches
    // the I2C bus (audit P1.5/P2.19). pcaFailedBoard is a board bucket, 0xFF = none.
    bool pcaUsed = false;
    bool pcaHealthy = true;
    uint8_t pcaFailedBoard = 0xFF;
};

class Diagnostics {
public:
    // Cumulative or latched setters. midiDropped/udpDropped/faults come from
    // components that already keep a running total, so they are SET, not added.
    void addMidiEvents(uint32_t n) { c_.midiEvents += n; }
    void setMidiDropped(uint32_t n) { c_.midiDropped = n; }
    void setUdpDropped(uint32_t n) { c_.udpDropped = n; }
    void setUdpRejected(uint32_t n) { c_.udpRejected = n; }
    void setFaults(uint32_t n) { c_.faults = n; }
    void setServoMoves(uint32_t n) { c_.servoMoves = n; }
    void setAxisMoves(uint32_t n) { c_.axisMoves = n; }
    void addHomingFailure() { ++c_.homingFailures; }
    void addLimitTrip() { ++c_.limitTrips; }
    void addMoveTimeout() { ++c_.moveTimeouts; }
    void setGovernorThrottles(uint32_t n) { c_.governorThrottles = n; }
    void setMoveMix(uint32_t deadline, uint32_t staggerableGrants) {
        c_.deadlineMoves = deadline;
        c_.staggerableGrants = staggerableGrants;
    }
    void addWifiReconnect() { ++c_.wifiReconnects; }
    void setPca(bool used, bool healthy, uint8_t failedBoard) {
        c_.pcaUsed = used;
        c_.pcaHealthy = healthy;
        c_.pcaFailedBoard = failedBoard;
    }

    // Track the deepest the command queue has been (a persistent high-water mark).
    void observeCmdQueueDepth(uint32_t depth) {
        if (depth > c_.cmdQueueHighWater) c_.cmdQueueHighWater = depth;
    }

    // Feed one loop() period (µs since the previous tick). Tracks the worst latency
    // and a jitter estimate = the largest deviation of a period from the running
    // mean (integer EMA, alpha = 1/8). The first sample only seeds the mean.
    void observeSchedulerPeriodUs(uint32_t dtUs) {
        if (dtUs > c_.schedulerMaxLatencyUs) c_.schedulerMaxLatencyUs = dtUs;
        if (!haveMean_) {
            mean_ = dtUs;
            haveMean_ = true;
        } else {
            uint32_t dev = dtUs > mean_ ? dtUs - mean_ : mean_ - dtUs;
            if (dev > c_.schedulerJitterUs) c_.schedulerJitterUs = dev;
            // EMA: mean += (dt - mean) / 8, in signed arithmetic.
            mean_ = static_cast<uint32_t>(
                static_cast<int32_t>(mean_) +
                (static_cast<int32_t>(dtUs) - static_cast<int32_t>(mean_)) / 8);
        }
        c_.schedulerMeanUs = mean_;
    }

    const DiagCounters& counters() const { return c_; }

    // Clear everything (e.g. a diagnostics reset from the web). Rarely needed.
    void reset() {
        c_ = DiagCounters{};
        mean_ = 0;
        haveMean_ = false;
    }

private:
    DiagCounters c_;
    uint32_t mean_ = 0;
    bool haveMean_ = false;
};

}  // namespace gmb
