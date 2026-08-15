// Runtime check for UDP MIDI ORIGIN IDENTITY.
//
// The peer table is bounded, so origins are reused. The first version of that
// reasoning was that reuse "can only confuse two peers with each other, which is
// the behaviour of not having ids at all" — and that is wrong in the one direction
// that leaves a finger pressed on a ringing string:
//
//     PC A -> NoteOn  ch1 C4     recorded as (origin 4, ch1, C4)
//     ...  13 more endpoints appear, A's slot is recycled
//     PC A -> NoteOff ch1 C4     A is new again: (origin 7, ch1, C4)
//                                -> no match, the Note Off is lost
//
// Without ids, A's key was shared but STABLE. Recycling changes ONE sender's
// identity between its own two halves, which the earlier design could not do.
//
// This drives the REAL MidiWifi peer table (it is outside the ARDUINO guard, so no
// socket is involved) and asserts the three properties that keep a Note Off matched
// to its Note On:
//
//   1. a sender that keeps talking keeps its origin, however many others appear;
//   2. a slot is never taken silently — losing one is REPORTED, so the owner can
//      release whatever that identity left sounding;
//   3. a silent peer's slot is reclaimed by the idle timeout, so the table does not
//      simply fill up with dead reconnects and start evicting live players.
//
// What it cannot prove: that ten minutes is the right idle timeout for a real
// rehearsal. That is a judgement, not a fact, and it is stated where it is set.
#include <cstdio>
#include <set>
#include <vector>

#include <WiFiUdp.h>

#include "../../src/platform/esp32/MidiWifi.h"

using namespace gmb;

static int g_fail = 0;
static const char* g_case = "";
static void beginCase(const char* n) { g_case = n; }
static void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("  [FAIL] %s — %s\n", g_case, what);
        ++g_fail;
    }
}

static UdpSource peer(uint32_t ip, uint16_t port) { return UdpSource{ip, port}; }

// poll() handles at most kMaxPacketsPerTick datagrams per pass, so a burst needs
// several loop turns — exactly as on the device, where the cap exists so a flood
// cannot stall the control loop. Drain the whole inbox before asserting.
static void pump(MidiWifi& m, uint32_t nowUs) {
    for (int i = 0; i < 16 && !gmbUdpInbox().empty(); ++i) m.poll(nowUs);
}

int main() {
    std::printf("UDP MIDI origin identity check\n");

    // ---- a talking peer keeps its origin -------------------------------------
    //
    // The property that matters. Twelve slots and thirteen endpoints used to mean
    // the first one silently became somebody else mid-note.
    {
        beginCase("a sender that keeps playing keeps its origin");
        MidiWifi m;
        uint32_t t = 1000;
        UdpSource a = peer(0x0A000001, 5006);
        uint8_t originA = m.originFor(a, t);

        // Twenty other endpoints arrive — well past the twelve slots — while A
        // keeps sending. A must not be the one evicted: it is the most recently
        // used, not the least.
        for (int i = 0; i < 20; ++i) {
            t += 10;
            m.originFor(peer(0x0A000002 + static_cast<uint32_t>(i), 5006), t);
            t += 10;
            check(m.originFor(a, t) == originA, "A's origin is unchanged");
        }
        check(m.originFor(a, t) == originA, "and still unchanged at the end");
    }

    // ---- the exact case from the audit ---------------------------------------
    //
    // A plays a note and then goes quiet. Enough other endpoints appear to take
    // every slot. A's identity IS lost — the table is bounded and something has to
    // give — but it must not be lost SILENTLY: the eviction is reported so the
    // owner releases what A left sounding.
    {
        beginCase("an evicted peer is reported, not dropped");
        MidiWifi m;
        uint32_t t = 1000;
        UdpSource a = peer(0x0A000001, 5006);
        uint8_t originA = m.originFor(a, t);
        m.takeReleasedOrigins();   // nothing pending yet

        // 12 fresh endpoints, each newer than A, so A is the least recently used.
        for (int i = 0; i < static_cast<int>(MidiOrigin::kNetworkPeerCount); ++i) {
            t += 10;
            m.originFor(peer(0x0B000001 + static_cast<uint32_t>(i), 6000), t);
        }
        auto released = m.takeReleasedOrigins();
        bool sawA = false;
        for (uint8_t o : released) if (o == originA) sawA = true;
        check(sawA, "A's origin is reported as released");
        // Draining is destructive: the owner must not be handed it twice and
        // release a live peer's notes on the second pass.
        check(m.takeReleasedOrigins().empty(), "and only reported once");

        // A comes back. It is a new sender now — which is exactly why the release
        // had to happen: its Note Off would not have matched its Note On.
        t += 10;
        check(m.originFor(a, t) != originA, "A returns under a different origin");
    }

    // ---- eviction takes the QUIETEST peer, not the next one round a ring ------
    //
    // Round-robin could evict a peer that is mid-note while an idle slot sat
    // untouched. Least-recently-used picks the one least likely to have anything
    // sounding, which is both fairer and cheaper in released notes.
    {
        beginCase("eviction takes the least recently used peer");
        MidiWifi m;
        uint32_t t = 1000;
        std::vector<uint8_t> origins;
        std::vector<UdpSource> peers;
        for (int i = 0; i < static_cast<int>(MidiOrigin::kNetworkPeerCount); ++i) {
            peers.push_back(peer(0x0C000001 + static_cast<uint32_t>(i), 7000));
            origins.push_back(m.originFor(peers.back(), t));
            t += 10;
        }
        // Everyone except the FIRST one speaks again, so slot 0 is the quietest.
        for (size_t i = 1; i < peers.size(); ++i) {
            t += 10;
            m.originFor(peers[i], t);
        }
        m.takeReleasedOrigins();
        t += 10;
        uint8_t fresh = m.originFor(peer(0x0D000001, 8000), t);
        check(fresh == origins[0], "the new peer took the quietest slot");
        auto released = m.takeReleasedOrigins();
        check(released.size() == 1 && released[0] == origins[0],
              "and exactly that origin was reported released");
    }

    // ---- the idle timeout keeps the table from silting up --------------------
    //
    // Without it the table only ever fills: a client that reconnects on a fresh
    // ephemeral port is a new (ip, port) every time, so twelve of those is an
    // afternoon rather than a concert — and then live players start being evicted.
    {
        beginCase("a long-silent peer's slot is reclaimed");
        MidiWifi m;
        uint32_t t = 1000;
        std::vector<uint8_t> origins;
        for (int i = 0; i < static_cast<int>(MidiOrigin::kNetworkPeerCount); ++i) {
            origins.push_back(m.originFor(peer(0x0E000001 + static_cast<uint32_t>(i), 9000), t));
        }
        m.takeReleasedOrigins();

        // Long after everyone went quiet, one new sender arrives.
        t += MidiWifi::kPeerIdleTimeoutMs + 1000;
        uint8_t fresh = m.originFor(peer(0x0F000001, 9100), t);
        auto released = m.takeReleasedOrigins();
        // Every stale slot is reaped, and the newcomer takes one of them: it is
        // never refused, and it never has to evict a peer that was still active.
        check(released.size() == MidiOrigin::kNetworkPeerCount,
              "every idle peer was retired and reported");
        bool reused = false;
        for (uint8_t o : origins) if (o == fresh) reused = true;
        check(reused, "the newcomer reused a reclaimed slot");
    }

    // ---- distinct senders still get distinct origins --------------------------
    // The reason any of this exists: without it, one host's Note Off releases
    // another host's string.
    {
        beginCase("distinct endpoints get distinct origins");
        MidiWifi m;
        std::set<uint8_t> seen;
        for (int i = 0; i < static_cast<int>(MidiOrigin::kNetworkPeerCount); ++i)
            seen.insert(m.originFor(peer(0x0A000001, static_cast<uint16_t>(5000 + i)), 1000));
        check(seen.size() == MidiOrigin::kNetworkPeerCount,
              "the same IP on different ports is different senders");
        for (uint8_t o : seen)
            check(o >= MidiOrigin::kFirstNetworkPeer && o < MidiOrigin::kMaxOrigins,
                  "and every origin is inside the network-peer range");
    }

    // ---- a junk datagram must claim nothing --------------------------------
    //
    // originFor() can EVICT, and an eviction releases whatever that peer left
    // sounding. Calling it before the datagram is known to be MIDI meant one stray
    // packet — a port scan, a broadcast — could take a live controller's slot and
    // damp its strings. These cases drive the REAL poll() over a UDP stub, because
    // the defect is an ORDER inside it and nothing below poll() can show it.
    {
        beginCase("a datagram that is not MIDI claims no origin");
        MidiWifi m;
        m.begin(5006);
        gmbUdpInbox().clear();
        // Fill every slot with real senders playing real notes.
        std::vector<uint8_t> noteOn = {0x90, 60, 100};
        for (int i = 0; i < static_cast<int>(MidiOrigin::kNetworkPeerCount); ++i) {
            gmbUdpPush(0x0A000001 + static_cast<uint32_t>(i), 5100, noteOn);
        }
        pump(m, 1000000);
        check(m.events().size() == MidiOrigin::kNetworkPeerCount,
              "every real sender was decoded");
        m.clear();
        m.takeReleasedOrigins();

        // A thirteenth host sends rubbish. It must not evict anyone.
        std::vector<uint8_t> junk = {0x00, 0x01, 0x02, 0x03};
        gmbUdpPush(0x0BADBEEF, 9999, junk);
        pump(m, 2000000);
        check(m.events().empty(), "the junk produced no events");
        check(m.takeReleasedOrigins().empty(),
              "and evicted nobody — no notes were released");

        // The same host then sends real MIDI, and NOW it may take a slot.
        gmbUdpPush(0x0BADBEEF, 9999, noteOn);
        pump(m, 3000000);
        check(m.events().size() == 1, "its real message was decoded");
        check(m.takeReleasedOrigins().size() == 1,
              "and only now does it cost somebody their slot");
    }

    // Events carry the origin of the host that actually sent them, stamped after
    // the parse rather than before it.
    {
        beginCase("events are stamped with their sender's origin");
        MidiWifi m;
        m.begin(5006);
        gmbUdpInbox().clear();
        std::vector<uint8_t> noteOn = {0x90, 60, 100};
        gmbUdpPush(0x0C000001, 5200, noteOn);
        gmbUdpPush(0x0C000002, 5200, noteOn);
        pump(m, 1000000);
        check(m.events().size() == 2, "both were decoded");
        if (m.events().size() == 2) {
            check(m.events()[0].origin != m.events()[1].origin,
                  "two hosts get two origins");
            for (const auto& e : m.events())
                check(e.origin >= MidiOrigin::kFirstNetworkPeer &&
                          e.origin < MidiOrigin::kMaxOrigins,
                      "and both are inside the network-peer range");
        }
    }

    // ---- expiry happens on its own, not only when somebody new arrives -------
    //
    // The reap used to live inside originFor(), so the documented ten minutes was
    // really "ten minutes AND then only once a new sender turns up". A peer that
    // vanished mid-set kept its origin for as long as the network stayed quiet.
    {
        beginCase("a peer expires with no new sender to trigger it");
        MidiWifi m;
        m.begin(5006);
        gmbUdpInbox().clear();
        gmbUdpPush(0x0D000001, 5300, {0x90, 60, 100});
        pump(m, 1000000);
        uint8_t origin = m.events().empty() ? 0 : m.events()[0].origin;
        check(origin != 0, "the peer got an origin");
        m.clear();
        m.takeReleasedOrigins();

        // Nothing arrives at all — just time passing and the loop running.
        m.poll(1000000ULL + (MidiWifi::kPeerIdleTimeoutMs + 1000ULL) * 1000ULL);
        auto released = m.takeReleasedOrigins();
        check(released.size() == 1 && released[0] == origin,
              "its slot was reclaimed and reported without any new traffic");
    }

    if (g_fail == 0) std::printf("\noriginscheck OK\n");
    else std::printf("\noriginscheck: %d failure(s)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
