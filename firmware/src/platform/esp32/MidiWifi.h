// Wi-Fi MIDI transport (spec §8.2). Initial implementation uses a
// configurable UDP socket carrying raw MIDI bytes; decoded events and SysEx come
// out through the transport-independent MidiParser so BLE/USB/DIN can be added
// later without touching the instrument logic (§8.3).
//
// Each poll() processes a BOUNDED number of packets so a UDP flood cannot
// monopolise loop() or starve the E-stop / LIMIT checks. Every SysEx request is
// tagged with its sender so the response goes back to the right client.
#pragma once

#include <cstdint>
#include <vector>

#include "../../core/midi/MidiIdentity.h"
#include "../../core/midi/MidiParser.h"
#include "../../core/midi/MidiTransport.h"
#include "../../core/net/UdpSourceGate.h"

#if defined(ARDUINO)
#include <WiFiUdp.h>
#endif

namespace gmb {

struct SysExPacket {
    std::vector<uint8_t> bytes;
#if defined(ARDUINO)
    IPAddress ip;
#endif
    uint16_t port = 0;
};

// Wi-Fi UDP MIDI transport (the MidiUdpTransport of spec §8.3). Conforms to the
// transport-neutral MidiTransport interface so it can feed the InstrumentController
// alongside a future USB / DIN / BLE transport, while keeping its UDP-specific SysEx
// reply / notify back-channel.
class MidiWifi : public MidiTransport {
public:
    void begin(uint16_t port = 5006);

    // Pull a bounded number of received packets and decode them.
    void poll(uint32_t nowUs) override;

    // Decoded channel-voice events for this poll (consume then clear()).
    std::vector<MidiEvent>& events() override { return events_; }
    // Complete SysEx requests received this poll, each with its sender.
    std::vector<SysExPacket>& sysexPackets() { return sysex_; }
    void clear() override {
        events_.clear();
        sysex_.clear();
    }
    MidiSource source() const override { return MidiSource::WifiUdp; }
    const char* name() const override { return "wifiUdp"; }

    // Reply to a specific SysEx sender.
    void reply(const SysExPacket& to, const uint8_t* data, size_t len);

    // Send an UNSOLICITED message (e.g. a capabilities-changed notification) to
    // the last host that made a SysEx request. Returns false if no host is known
    // yet. Used so a runtime capability change is pushed, not only polled.
    bool notifyLastSender(const uint8_t* data, size_t len);
    bool hasLastSender() const { return lastSenderPort_ != 0; }

    // Origins whose peer slot was evicted or expired during the last poll(). The
    // owner must release each one on the instrument (see releaseOrigin), because
    // those senders no longer exist and nothing they left active will ever be
    // matched again. Drained by the call.
    std::vector<uint8_t> takeReleasedOrigins() {
        std::vector<uint8_t> out;
        out.swap(releasedOrigins_);
        return out;
    }

    // The origin this (ip, port) has right now, allocating or recycling a slot as
    // needed and stamping it as seen at `nowMs`. Public because it IS the
    // transport's identity contract — "the same sender keeps the same origin, and a
    // sender that loses its slot is reported as released" is the property that
    // keeps a Note Off matched to its Note On, and a property nothing can call is a
    // property nothing can check.
    uint8_t originFor(const UdpSource& src, uint32_t nowMs);

    // How long a silent peer keeps its origin. Long enough that a pause between
    // pieces is not an eviction, short enough that a laptop closed an hour ago is
    // not still holding a slot against a live player. Anything it left sounding is
    // released when the slot goes, so expiry is never how a note gets stuck.
    static constexpr uint32_t kPeerIdleTimeoutMs = 10u * 60u * 1000u;  // 10 minutes

    // Dropped-input counters (oversized datagrams / per-tick event overflow) so a
    // sustained MIDI overflow is observable rather than silent.
    uint32_t droppedEvents() const { return droppedEvents_; }
    uint32_t droppedPackets() const { return droppedPackets_; }

    // UDP source posture (P1.11). Default Open = accept any sender (unchanged
    // behaviour). LockToFirst pins the session to the first sender; Disabled refuses
    // all UDP. INERT until a runtime/DeviceConfig calls setSourcePolicy — the reject
    // path is exercised by host tests, but the live-socket behaviour needs bench
    // validation. rejectedCount() surfaces datagrams the gate refused (diagnostics).
    void setSourcePolicy(UdpSourcePolicy p) { gate_.setPolicy(p); }
    UdpSourcePolicy sourcePolicy() const { return gate_.policy(); }
    // Forget the locked session so the next accepted datagram re-locks (the
    // Settings "Unlock current sender" action, audit 4 P2.3).
    void unlockSource() { gate_.unlock(); }
    bool sourceLocked() const { return gate_.locked(); }
    uint32_t rejectedPackets() const { return gate_.rejectedCount(); }

private:
    static constexpr int kMaxPacketsPerTick = 8;
    static constexpr size_t kMaxEventsPerTick = 128;

    MidiParser parser_;
    uint16_t port_ = 5006;
    std::vector<MidiEvent> events_;
    std::vector<SysExPacket> sysex_;
    uint16_t lastSenderPort_ = 0;
    uint32_t droppedEvents_ = 0;
    uint32_t droppedPackets_ = 0;
    UdpSourceGate gate_;  // P1.11 source posture (Open by default -> no behaviour change)

    // ---- per-peer origins ----------------------------------------------------
    //
    // MidiSource::WifiUdp names the TRANSPORT, and the default policy accepts any
    // sender, so two hosts on the same network are otherwise indistinguishable —
    // and one host's Note Off releases the other's note. Each (IP, port) therefore
    // gets a small origin id, which is what the note key is actually built from.
    //
    // The table is bounded, so ids get REUSED, and the first version of this
    // reasoned that reuse "can only confuse two peers with each other, which is the
    // behaviour of not having ids at all". That was wrong, and wrong in the
    // direction that leaves a finger on a string:
    //
    //     PC A  -> NoteOn ch1 C4        recorded as (origin 4, ch1, C4)
    //     ...   13 more endpoints appear, A's slot is recycled
    //     PC A  -> NoteOff ch1 C4       A is new again: (origin 7, ch1, C4)
    //                                   -> no match, the Note Off is lost
    //
    // Without ids, A's key was stable — shared with others, but stable. Recycling
    // changes ONE sender's identity between its own Note On and its Note Off, which
    // the earlier design could not do. And it does not take 13 simultaneous
    // players: a client that reconnects on a new ephemeral port each time is a new
    // (ip, port) every time, and nothing ever expired.
    //
    // Three things fix it, and all three are needed:
    //   * a last-seen stamp, so eviction takes the LEAST RECENTLY USED peer instead
    //     of the next one round the ring — which could evict a peer that is
    //     mid-note while an idle slot sat unused;
    //   * an idle timeout, so slots free themselves and the common case (serial
    //     reconnects) never reaches eviction at all;
    //   * releaseOrigin() on the way out: whatever the evicted peer left active is
    //     released, because that identity is gone. A note that outlives its sender
    //     is a stuck note whatever the table size.
    struct Peer {
        uint32_t ip = 0;
        uint16_t port = 0;
        bool used = false;
        uint32_t lastSeenMs = 0;
    };
    Peer peers_[MidiOrigin::kNetworkPeerCount];
    void retirePeer(uint8_t slot);
    // Origins whose peer was evicted or expired since the last call. The owner
    // drains this each loop and releases each one on the instrument; MidiWifi has
    // no business knowing what a note is.
    std::vector<uint8_t> releasedOrigins_;
#if defined(ARDUINO)
    WiFiUDP udp_;
    IPAddress lastSenderIp_;
    uint8_t buf_[512];
#endif
};

}  // namespace gmb
