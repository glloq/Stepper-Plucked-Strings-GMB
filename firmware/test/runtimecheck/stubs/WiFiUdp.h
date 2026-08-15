// A WiFiUDP stub you can actually feed.
//
// The shared hostcheck stub answers parsePacket() with 0 forever, which is enough
// to type-check MidiWifi and nothing else — poll() never runs, so the ORDER of what
// it does is untestable. That order is exactly what matters: whether a peer slot is
// claimed (and another peer evicted, and its notes released) before or after the
// datagram is known to be MIDI.
//
// This one holds a queue of datagrams the harness pushes in, each with its sender,
// and hands them to poll() the way a socket would.
#pragma once
#include <Arduino.h>
#include <WiFi.h>

#include <cstring>
#include <deque>
#include <vector>

struct GmbUdpDatagram {
    uint32_t ip = 0;
    uint16_t port = 0;
    std::vector<uint8_t> bytes;
};

// The queue the harness fills; MidiWifi drains it through the class below.
inline std::deque<GmbUdpDatagram>& gmbUdpInbox() {
    static std::deque<GmbUdpDatagram> q;
    return q;
}
inline void gmbUdpPush(uint32_t ip, uint16_t port, const std::vector<uint8_t>& bytes) {
    gmbUdpInbox().push_back(GmbUdpDatagram{ip, port, bytes});
}

class WiFiUDP {
public:
    void begin(uint16_t) {}

    // Present the head of the queue. Returns its length, 0 when empty — the same
    // contract poll() is written against.
    int parsePacket() {
        if (gmbUdpInbox().empty()) { current_ = GmbUdpDatagram{}; return 0; }
        current_ = gmbUdpInbox().front();
        gmbUdpInbox().pop_front();
        pending_ = true;
        return static_cast<int>(current_.bytes.size());
    }
    int read(uint8_t* out, size_t max) {
        if (!pending_) return 0;
        size_t n = current_.bytes.size() < max ? current_.bytes.size() : max;
        std::memcpy(out, current_.bytes.data(), n);
        pending_ = false;
        return static_cast<int>(n);
    }
    void flush() { pending_ = false; }
    void clear() { pending_ = false; }
    IPAddress remoteIP() { return IPAddress(current_.ip); }
    uint16_t remotePort() { return current_.port; }
    void beginPacket(IPAddress, uint16_t) {}
    size_t write(const uint8_t*, size_t n) { return n; }
    void endPacket() {}

private:
    GmbUdpDatagram current_;
    bool pending_ = false;
};
