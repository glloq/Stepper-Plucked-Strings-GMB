#include "MidiWifi.h"

namespace gmb {

void MidiWifi::begin(uint16_t port) {
    port_ = port;
    parser_.setSource(MidiSource::WifiUdp);
#if defined(ARDUINO)
    udp_.begin(port_);
#endif
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
