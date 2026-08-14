// UDP MIDI source gate (audit P1.11) — the "recognized session / disable UDP" posture.
//
// A pure, transport-agnostic policy deciding whether a UDP MIDI datagram from a given
// source (IPv4 + port) may be processed. It exists so the Performance posture
// (NETWORK_HOTSPOT.md §7) can, once wired, refuse stray UDP traffic:
//
//   Open        - accept any source (Setup mode / default; the current behaviour).
//   LockToFirst - accept the first source seen, then ONLY that one: a rogue device on
//                 the same network cannot inject notes once a controller is talking
//                 ("session reconnue").
//   Disabled    - refuse all UDP (Performance mode with UDP turned off).
//
// This is the host-testable KERNEL. MidiWifi holds one but leaves it inert (policy
// Open) until a DeviceConfig/runtime exposes the posture. Even then the network
// behaviour needs on-device validation — nothing here is proven on a live socket.
#pragma once

#include <cstdint>

namespace gmb {

enum class UdpSourcePolicy : uint8_t {
    Open,         // accept any source (default)
    LockToFirst,  // accept the first source, then only that one (recognized session)
    Disabled,     // reject every datagram
};

inline const char* udpSourcePolicyName(UdpSourcePolicy p) {
    switch (p) {
        case UdpSourcePolicy::Open:        return "open";
        case UdpSourcePolicy::LockToFirst: return "lockToFirst";
        case UdpSourcePolicy::Disabled:    return "disabled";
    }
    return "open";
}

// A source identity. IPv4 is kept as a raw 32-bit value because equality is all the
// gate needs — it never interprets the address, so byte order is irrelevant.
struct UdpSource {
    uint32_t ip = 0;
    uint16_t port = 0;
    bool operator==(const UdpSource& o) const { return ip == o.ip && port == o.port; }
    bool operator!=(const UdpSource& o) const { return !(*this == o); }
};

class UdpSourceGate {
public:
    // Changing the policy starts a fresh session (any existing lock is cleared), so
    // entering LockToFirst always re-locks to the next accepted datagram.
    void setPolicy(UdpSourcePolicy p) {
        if (p != policy_) { policy_ = p; unlock(); }
    }
    UdpSourcePolicy policy() const { return policy_; }

    // Decide whether a datagram from `src` may be processed. Under LockToFirst the
    // first accepted source becomes the locked session; a later different source is
    // refused. Every refusal bumps rejectedCount() so a misconfig / injection attempt
    // is observable rather than silent.
    bool accept(const UdpSource& src) {
        switch (policy_) {
            case UdpSourcePolicy::Disabled:
                break;  // reject
            case UdpSourcePolicy::Open:
                return true;
            case UdpSourcePolicy::LockToFirst:
                if (!locked_) { locked_ = true; lockedSrc_ = src; return true; }
                if (src == lockedSrc_) return true;
                break;  // different source -> reject
        }
        ++rejected_;
        return false;
    }

    // Non-locking probe + explicit lock, for a caller that must validate the
    // PAYLOAD before adopting a sender as the locked session: under LockToFirst
    // any stray UDP packet used to claim the lock before being parsed (audit 5).
    // wouldAccept() answers like accept() but never locks nor counts a rejection;
    // lockTo() then adopts the sender once its first datagram proved to be MIDI.
    bool wouldAccept(const UdpSource& src) const {
        switch (policy_) {
            case UdpSourcePolicy::Disabled: return false;
            case UdpSourcePolicy::Open: return true;
            case UdpSourcePolicy::LockToFirst: return !locked_ || src == lockedSrc_;
        }
        return false;
    }
    void lockTo(const UdpSource& src) {
        if (policy_ == UdpSourcePolicy::LockToFirst) { locked_ = true; lockedSrc_ = src; }
    }
    // Count a refusal decided by the CALLER (e.g. a non-MIDI datagram from a
    // candidate sender) so it stays observable like every gate rejection.
    void noteRejected() { ++rejected_; }

    bool locked() const { return locked_; }
    UdpSource lockedSource() const { return lockedSrc_; }
    // Forget the locked session (e.g. on a session timeout or an explicit unlock) so
    // the next accepted datagram re-establishes it.
    void unlock() { locked_ = false; lockedSrc_ = {}; }

    uint32_t rejectedCount() const { return rejected_; }
    void resetCounters() { rejected_ = 0; }

private:
    UdpSourcePolicy policy_ = UdpSourcePolicy::Open;
    bool locked_ = false;
    UdpSource lockedSrc_;
    uint32_t rejected_ = 0;
};

}  // namespace gmb
