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
    int packet = udp_.parsePacket();
    while (packet > 0) {
        lastRemoteIp_ = udp_.remoteIP();
        lastRemotePort_ = udp_.remotePort();
        int n = udp_.read(buf_, sizeof(buf_));
        if (n > 0) parser_.feed(buf_, static_cast<size_t>(n), nowUs);
        packet = udp_.parsePacket();
    }
#else
    (void)nowUs;
#endif
}

void MidiWifi::send(const uint8_t* data, size_t len) {
#if defined(ARDUINO)
    if (lastRemotePort_ == 0) return;
    udp_.beginPacket(lastRemoteIp_, lastRemotePort_);
    udp_.write(data, len);
    udp_.endPacket();
#else
    (void)data;
    (void)len;
#endif
}

}  // namespace gmb
