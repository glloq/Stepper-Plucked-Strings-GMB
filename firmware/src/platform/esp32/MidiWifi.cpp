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

    // Reap anything silent for longer than the idle timeout BEFORE looking for a
    // free slot. Without this the table only ever fills: a client that reconnects
    // on a fresh ephemeral port is a new (ip, port) every time, and twelve of those
    // is an afternoon, not a concert.
    for (uint8_t i = 0; i < MidiOrigin::kNetworkPeerCount; ++i) {
        if (!peers_[i].used) continue;
        if (static_cast<uint32_t>(nowMs - peers_[i].lastSeenMs) < kPeerIdleTimeoutMs) continue;
        retirePeer(i);
    }

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
void MidiWifi::retirePeer(uint8_t slot) {
    if (slot >= MidiOrigin::kNetworkPeerCount || !peers_[slot].used) return;
    peers_[slot] = Peer{};
    releasedOrigins_.push_back(static_cast<uint8_t>(MidiOrigin::kFirstNetworkPeer + slot));
}

void MidiWifi::poll(uint32_t nowUs) {
#if defined(ARDUINO)
    // Process at most kMaxPacketsPerTick packets this pass so a flood cannot
    // stall the control loop; the rest wait for the next poll().
    int packet = udp_.parsePacket();
    int handled = 0;
    while (packet > 0 && handled < kMaxPacketsPerTick) {
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
            packet = udp_.parsePacket();
            continue;
        }
        // Reject an oversized datagram ENTIRELY: reading only the first
        // sizeof(buf_) bytes would parse a truncated message (a Note Off past the
        // buffer would be lost, leaving a note stuck on). Discard and skip it.
        if (packet > static_cast<int>(sizeof(buf_))) {
            udp_.clear();  // discard the current datagram (flush() is deprecated)
            ++droppedPackets_;
            ++handled;
            packet = udp_.parsePacket();
            continue;
        }
        int n = udp_.read(buf_, sizeof(buf_));
        if (n > 0) {
            // Each UDP datagram is self-contained: drop any running status or
            // in-progress SysEx from a previous packet so one sender can never
            // continue/terminate another sender's message (shared-parser fix).
            parser_.resetStream();
            // Tag every event with WHICH host sent it, not just "the Wi-Fi".
            parser_.setOrigin(originFor(src, nowUs / 1000u));
            parser_.feed(buf_, static_cast<size_t>(n), nowUs);
            // LockToFirst, no session yet: only a datagram that actually decodes
            // as MIDI (events or SysEx) may adopt this sender as the locked
            // session; junk is discarded and counted, and the port stays open for
            // the real controller (audit 5).
            if (pendingLock) {
                if (parser_.events().empty() && parser_.sysex().empty()) {
                    gate_.noteRejected();
                    parser_.clear();
                    ++handled;
                    packet = udp_.parsePacket();
                    continue;
                }
                gate_.lockTo(src);
            }
            // Move decoded events out (bounded). Count anything dropped so a
            // sustained overflow is visible rather than silent.
            for (auto& e : parser_.events()) {
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
        packet = udp_.parsePacket();
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
