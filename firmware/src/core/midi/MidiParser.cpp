#include "MidiParser.h"

namespace gmb {

int MidiParser::expectedData(uint8_t status) {
    switch (status & 0xF0) {
        case 0x80:  // Note Off
        case 0x90:  // Note On
        case 0xA0:  // Poly aftertouch
        case 0xB0:  // Control change
        case 0xE0:  // Pitch bend
            return 2;
        case 0xC0:  // Program change
        case 0xD0:  // Channel aftertouch
            return 1;
        default:
            return 0;
    }
}

void MidiParser::feed(const uint8_t* data, size_t len, uint32_t nowUs) {
    for (size_t i = 0; i < len; ++i) feed(data[i], nowUs);
}

void MidiParser::feed(uint8_t b, uint32_t nowUs) {
    // System real-time messages (0xF8..0xFF) may interleave anywhere, including
    // inside a SysEx; ignore them here so they never corrupt the SysEx buffer.
    if (b >= 0xF8) return;

    // SysEx handling.
    if (inSysex_) {
        if (b == 0xF7) {
            sysbuf_.push_back(b);
            sysex_.push_back(sysbuf_);
            sysbuf_.clear();
            inSysex_ = false;
            return;
        }
        // Bound the buffer so a truncated / malicious stream cannot exhaust RAM.
        if (sysbuf_.size() >= kMaxSysExBytes) {
            sysbuf_.clear();
            inSysex_ = false;  // abort this SysEx; wait for the next 0xF0
            return;
        }
        sysbuf_.push_back(b);
        return;
    }
    if (b == 0xF0) {
        inSysex_ = true;
        sysbuf_.clear();
        sysbuf_.push_back(b);
        return;
    }

    if (b & 0x80) {
        // Status byte.
        if (b >= 0xF8) return;  // real-time messages: ignore here
        if (b >= 0xF1 && b <= 0xF6) {
            status_ = 0;  // system common: not handled, clears running status
            return;
        }
        status_ = b;
        dataCount_ = 0;
        expected_ = expectedData(b);
        if (expected_ == 0) emit(nowUs);  // status-only (none for voice msgs)
        return;
    }

    // Data byte (uses running status).
    if (status_ == 0) return;
    if (dataCount_ < 2) data_[dataCount_++] = b;
    if (expected_ == 0) expected_ = expectedData(status_);
    if (dataCount_ >= expected_) {
        emit(nowUs);
        dataCount_ = 0;  // ready for the next message under running status
    }
}

void MidiParser::emit(uint32_t nowUs) {
    MidiEvent e;
    e.timestampUs = nowUs;
    e.source = source_;
    e.type = status_ & 0xF0;
    e.channel = status_ & 0x0F;
    e.data1 = data_[0];
    e.data2 = expected_ >= 2 ? data_[1] : 0;
    events_.push_back(e);
}

}  // namespace gmb
