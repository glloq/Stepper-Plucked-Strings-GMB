#include "MidiWifi.h"

namespace gmb {

void MidiWifi::begin(uint16_t port) {
    port_ = port;
    parser_.setSource(MidiSource::WifiUdp);
#if defined(ARDUINO)
    udp_.begin(port_);
#endif
}

// The origin id for a sender, allocated on first sight.
uint8_t MidiWifi::originFor(const UdpSource& src, uint32_t nowMs) {
    // Known sender: refresh its stamp and keep its id. This is the only path that
    // matters for a note's identity — a sender's origin must not change between its
    // Note On and its Note Off, and the only way to guarantee that is never to take
    // a slot away from a peer that is still talking.
    for (uint8_t i = 0; i < MidiOrigin::kNetworkPeerCount; ++i) {
        if (peers_[i].used && peers_[i].ip == src.ip && peers_[i].port == src.port) {
            peers_[i].lastSeenMs = nowMs;
            return static_cast<uint8_t>(MidiOrigin::kFirstNetworkPeer + i);
        }
    }

    // Reap before looking for a free slot as well as from poll(): a datagram can
    // arrive after a long quiet spell, in the same pass that would reap.
    reapIdlePeers(nowMs);

    for (uint8_t i = 0; i < MidiOrigin::kNetworkPeerCount; ++i) {
        if (peers_[i].used) continue;
        peers_[i] = Peer{src.ip, src.port, true, nowMs};
        return static_cast<uint8_t>(MidiOrigin::kFirstNetworkPeer + i);
    }

    // Still full: evict the LEAST RECENTLY USED peer, not the next one round a
    // ring. Round-robin could take the slot of a peer that is mid-note while an
    // idle one sat untouched — choosing the quietest peer is both fairer and the
    // one least likely to have anything sounding.
    uint8_t victim = 0;
    for (uint8_t i = 1; i < MidiOrigin::kNetworkPeerCount; ++i)
        if (static_cast<int32_t>(peers_[i].lastSeenMs - peers_[victim].lastSeenMs) < 0)
            victim = i;
    retirePeer(victim);
    peers_[victim] = Peer{src.ip, src.port, true, nowMs};
    return static_cast<uint8_t>(MidiOrigin::kFirstNetworkPeer + victim);
}

// Free a slot and record that its origin must be released. The peer it named is
// gone as far as this device is concerned: its next datagram gets a different id,
// so its own Note Off can no longer match its own Note On. Whatever it left
// sounding has to be released here, or it is held until something unrelated
// happens to reuse that string.
// Free every slot silent for longer than the idle timeout. Without this the table
// only ever fills: a client that reconnects on a fresh ephemeral port is a new
// (ip, port) every time, and twelve of those is an afternoon, not a concert.
void MidiWifi::reapIdlePeers(uint32_t nowMs) {
    for (uint8_t i = 0; i < MidiOrigin::kNetworkPeerCount; ++i) {
        if (!peers_[i].used) continue;
        if (static_cast<uint32_t>(nowMs - peers_[i].lastSeenMs) < kPeerIdleTimeoutMs) continue;
        retirePeer(i);
    }
}

void MidiWifi::retirePeer(uint8_t slot) {
    if (slot >= MidiOrigin::kNetworkPeerCount || !peers_[slot].used) return;
    peers_[slot] = Peer{};
    releasedOrigins_.push_back(static_cast<uint8_t>(MidiOrigin::kFirstNetworkPeer + slot));
}

void MidiWifi::poll(uint32_t nowUs) {
    // Reclaim idle slots on every pass, not only when an unknown sender turns up.
    // The reap used to live inside originFor(), which meant the documented ten
    // minutes was really "ten minutes, and then only once somebody new arrives" —
    // a peer that vanished mid-set kept its origin for as long as the network
    // stayed quiet. Twelve comparisons per loop is not a cost worth a caveat.
    reapIdlePeers(nowUs / 1000u);
#if defined(ARDUINO)
    // Process at most kMaxPacketsPerTick packets this pass so a flood cannot
    // stall the control loop; the rest wait for the next poll().
    // Fetch INSIDE the loop, and only when there is room to process what comes
    // back. The fetch used to sit at the bottom of the body, so the pass that hit
    // the cap had already called parsePacket() one more time — making a datagram
    // current that nobody would read. The next poll() calls parsePacket() again,
    // which on the real socket DISCARDS it: one message silently lost out of every
    // nine, under exactly the burst the cap exists to survive. A Note Off is as
    // likely to be the lost one as anything else.
    //
    // Invisible until now because the host stub answered "no packet" forever, so
    // poll() was only ever compiled, never run.
    int handled = 0;
    while (handled < kMaxPacketsPerTick) {
        int packet = udp_.parsePacket();
        if (packet <= 0) break;
        IPAddress remoteIp = udp_.remoteIP();
        uint16_t remotePort = udp_.remotePort();
        UdpSource src{static_cast<uint32_t>(remoteIp), remotePort};
        // P1.11 source gate: with a non-Open posture, refuse a datagram from an
        // unrecognised sender BEFORE parsing it, so stray/rogue UDP never reaches
        // the note engine. Under LockToFirst the FIRST sender only becomes the
        // locked session once its datagram actually parses as MIDI — any random
        // UDP packet on the port used to claim the lock unparsed (audit 5).
        bool pendingLock =
            gate_.policy() == UdpSourcePolicy::LockToFirst && !gate_.locked();
        if (!pendingLock && !gate_.accept(src)) {
            udp_.clear();  // discard the refused datagram
            ++handled;
            continue;
        }
        // Reject an oversized datagram ENTIRELY: reading only the first
        // sizeof(buf_) bytes would parse a truncated message (a Note Off past the
        // buffer would be lost, leaving a note stuck on). Discard and skip it.
        if (packet > static_cast<int>(sizeof(buf_))) {
            udp_.clear();  // discard the current datagram (flush() is deprecated)
            ++droppedPackets_;
            ++handled;
            continue;
        }
        int n = udp_.read(buf_, sizeof(buf_));
        if (n > 0) {
            // Each UDP datagram is self-contained: drop any running status or
            // in-progress SysEx from a previous packet so one sender can never
            // continue/terminate another sender's message (shared-parser fix).
            parser_.resetStream();
            // PARSE FIRST, allocate an origin second.
            //
            // originFor() can EVICT another peer to make room, and an eviction
            // releases everything that peer left sounding. Calling it before the
            // datagram is known to be MIDI meant one junk packet from an unknown
            // host — a port scan, a stray broadcast — could take a real
            // controller's slot and damp its strings mid-phrase. Nothing about a
            // sender is worth recording until it has said something.
            //
            // The origin is therefore provisional here and stamped onto the events
            // below, once there are events. The lock does the same thing for the
            // same reason (audit 5); this is that rule applied to the peer table.
            parser_.setOrigin(MidiOrigin::kInternal);
            parser_.feed(buf_, static_cast<size_t>(n), nowUs);
            if (parser_.events().empty() && parser_.sysex().empty()) {
                // Not MIDI. No slot claimed, no peer evicted, nothing released.
                if (pendingLock) gate_.noteRejected();
                parser_.clear();
                ++handled;
                continue;
            }
            // LockToFirst, no session yet: only a datagram that actually decodes
            // as MIDI may adopt this sender as the locked session, so the port
            // stays open for the real controller (audit 5).
            if (pendingLock) gate_.lockTo(src);
            // It spoke, so now it gets an identity.
            uint8_t origin = originFor(src, nowUs / 1000u);
            // Move decoded events out (bounded). Count anything dropped so a
            // sustained overflow is visible rather than silent.
            for (auto& e : parser_.events()) {
                e.origin = origin;  // WHICH host sent it, not just "the Wi-Fi"
                if (events_.size() < kMaxEventsPerTick) events_.push_back(e);
                else ++droppedEvents_;
            }
            // Tag each SysEx from THIS packet with THIS sender.
            for (auto& s : parser_.sysex()) {
                SysExPacket p;
                p.bytes = s;
                p.ip = remoteIp;
                p.port = remotePort;
                sysex_.push_back(p);
            }
            parser_.clear();
        }
        ++handled;
    }
#else
    (void)nowUs;
#endif
}

void MidiWifi::reply(const SysExPacket& to, const uint8_t* data, size_t len) {
#if defined(ARDUINO)
    if (to.port == 0) return;
    udp_.beginPacket(to.ip, to.port);
    udp_.write(data, len);
    udp_.endPacket();
    // Remember this host so a later unsolicited notification can reach it.
    lastSenderIp_ = to.ip;
    lastSenderPort_ = to.port;
#else
    (void)to;
    (void)data;
    (void)len;
    lastSenderPort_ = to.port;
#endif
}

bool MidiWifi::notifyLastSender(const uint8_t* data, size_t len) {
    if (lastSenderPort_ == 0) return false;
#if defined(ARDUINO)
    udp_.beginPacket(lastSenderIp_, lastSenderPort_);
    udp_.write(data, len);
    udp_.endPacket();
    return true;
#else
    (void)data;
    (void)len;
    return true;
#endif
}

}  // namespace gmb
