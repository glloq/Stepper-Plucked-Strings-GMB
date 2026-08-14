// P1.11: the UDP MIDI source gate — accept/reject a datagram by (IPv4, port) under a
// posture (Open / LockToFirst / Disabled). Pins the "recognized session / disable UDP"
// security rule that MidiWifi holds inert until a runtime exposes it.
#include "TestFramework.h"
#include "../src/core/net/UdpSourceGate.h"

#include <string>

using namespace gmb;

static UdpSource src(uint32_t ip, uint16_t port) { return UdpSource{ip, port}; }

TEST(udp_gate_open_accepts_everything) {
    UdpSourceGate g;  // default policy is Open
    CHECK(g.policy() == UdpSourcePolicy::Open);
    CHECK(g.accept(src(0x0A000001, 5006)));
    CHECK(g.accept(src(0x0A000002, 5006)));   // a different source is still fine
    CHECK(g.accept(src(0xC0A80105, 40000)));
    CHECK(g.rejectedCount() == 0);
    CHECK(!g.locked());
}

TEST(udp_gate_disabled_rejects_everything) {
    UdpSourceGate g;
    g.setPolicy(UdpSourcePolicy::Disabled);
    CHECK(!g.accept(src(0x0A000001, 5006)));
    CHECK(!g.accept(src(0x0A000001, 5006)));
    CHECK(g.rejectedCount() == 2);            // every refusal is counted
}

TEST(udp_gate_lock_to_first_pins_the_session) {
    UdpSourceGate g;
    g.setPolicy(UdpSourcePolicy::LockToFirst);
    CHECK(!g.locked());
    // First datagram is accepted and becomes the locked session.
    CHECK(g.accept(src(0x0A000001, 5006)));
    CHECK(g.locked());
    CHECK(g.lockedSource() == src(0x0A000001, 5006));
    // Same source keeps flowing.
    CHECK(g.accept(src(0x0A000001, 5006)));
    // A different IP is refused...
    CHECK(!g.accept(src(0x0A000002, 5006)));
    // ...and so is the same IP on a different port (port is part of the identity).
    CHECK(!g.accept(src(0x0A000001, 5007)));
    CHECK(g.rejectedCount() == 2);
    // The locked source is unchanged by the rejected traffic.
    CHECK(g.lockedSource() == src(0x0A000001, 5006));
}

TEST(udp_gate_unlock_lets_a_new_session_form) {
    UdpSourceGate g;
    g.setPolicy(UdpSourcePolicy::LockToFirst);
    CHECK(g.accept(src(0x0A000001, 5006)));
    g.unlock();                               // e.g. a session timeout
    CHECK(!g.locked());
    // The next source now becomes the new locked session.
    CHECK(g.accept(src(0x0A000009, 5006)));
    CHECK(g.lockedSource() == src(0x0A000009, 5006));
    CHECK(!g.accept(src(0x0A000001, 5006)));  // the OLD source is now the stranger
}

TEST(udp_gate_changing_policy_starts_a_fresh_session) {
    UdpSourceGate g;
    g.setPolicy(UdpSourcePolicy::LockToFirst);
    CHECK(g.accept(src(0x0A000001, 5006)));   // lock to .1
    CHECK(g.locked());
    // Bouncing through Open and back to LockToFirst must clear the old lock.
    g.setPolicy(UdpSourcePolicy::Open);
    CHECK(!g.locked());
    g.setPolicy(UdpSourcePolicy::LockToFirst);
    CHECK(!g.locked());
    CHECK(g.accept(src(0x0A000002, 5006)));    // re-locks to .2, not the stale .1
    CHECK(g.lockedSource() == src(0x0A000002, 5006));
    // Re-selecting the SAME policy is a no-op (does not drop the live session).
    g.setPolicy(UdpSourcePolicy::LockToFirst);
    CHECK(g.locked());
    CHECK(g.lockedSource() == src(0x0A000002, 5006));
}

TEST(udp_gate_policy_names_are_stable) {
    CHECK(std::string(udpSourcePolicyName(UdpSourcePolicy::Open)) == "open");
    CHECK(std::string(udpSourcePolicyName(UdpSourcePolicy::LockToFirst)) == "lockToFirst");
    CHECK(std::string(udpSourcePolicyName(UdpSourcePolicy::Disabled)) == "disabled");
}

// Payload-validated locking (audit 5): wouldAccept() probes without locking, and
// lockTo() adopts the sender only once its datagram proved to be MIDI — a stray
// UDP packet can no longer claim the LockToFirst session unparsed.
TEST(udp_gate_would_accept_probes_without_locking) {
    UdpSourceGate g;
    g.setPolicy(UdpSourcePolicy::LockToFirst);
    UdpSource junk{0x0A000001, 4000};
    UdpSource ctrl{0x0A000002, 5004};
    CHECK(g.wouldAccept(junk));
    CHECK(!g.locked());              // probing never locks
    g.noteRejected();                // caller refused the junk payload
    CHECK_EQ((int)g.rejectedCount(), 1);
    CHECK(g.wouldAccept(ctrl));      // the port is still open for the real sender
    g.lockTo(ctrl);
    CHECK(g.locked());
    CHECK(!g.wouldAccept(junk));     // now a different source is refused
    CHECK(g.wouldAccept(ctrl));
}
