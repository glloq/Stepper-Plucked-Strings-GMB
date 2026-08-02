#include "GmbSysEx.h"

namespace gmb {

namespace {

inline uint8_t b7(int v) { return static_cast<uint8_t>(v & 0x7F); }

void header(std::vector<uint8_t>& m, SysExBlock block, SysExDirection dir) {
    m.push_back(GmbSysEx::kStart);
    m.push_back(GmbSysEx::kManufacturer);
    m.push_back(GmbSysEx::kGmbId);
    m.push_back(static_cast<uint8_t>(block));
    m.push_back(static_cast<uint8_t>(dir));
}

// Encode a 32-bit revision as 5 big-endian 7-bit bytes (35-bit capacity).
void pushRevision(std::vector<uint8_t>& m, uint32_t rev) {
    for (int i = 4; i >= 0; --i) m.push_back(b7(rev >> (7 * i)));
}

}  // namespace

std::vector<uint8_t> GmbSysEx::encodeIdentity(const CapabilitySnapshot& s) {
    std::vector<uint8_t> m;
    header(m, SysExBlock::Identity, SysExDirection::Response);
    m.push_back(b7(s.identity.version));
    for (int i = 0; i < 5; ++i) m.push_back(b7(s.identity.deviceId[i]));
    // Device name padded / truncated to 32 bytes.
    for (int i = 0; i < 32; ++i) {
        char c = i < static_cast<int>(s.identity.deviceName.size())
                     ? s.identity.deviceName[i]
                     : 0;
        m.push_back(b7(static_cast<uint8_t>(c)));
    }
    for (int i = 0; i < 3; ++i) m.push_back(b7(s.identity.firmware[i]));
    // Features field: 5 bytes, first carries the flags.
    m.push_back(b7(s.identity.features));
    for (int i = 0; i < 4; ++i) m.push_back(0);
    m.push_back(kEnd);
    return m;
}

std::vector<uint8_t> GmbSysEx::encodeDescriptor(const CapabilitySnapshot& s) {
    std::vector<uint8_t> m;
    header(m, SysExBlock::Descriptor, SysExDirection::Response);
    m.push_back(0x01);  // instrument count
    m.push_back(0x01);  // instrument index
    m.push_back(b7(s.descriptor.channel));
    m.push_back(b7(s.descriptor.gmProgram));
    m.push_back(b7(s.descriptor.typeId));
    m.push_back(kEnd);
    return m;
}

std::vector<uint8_t> GmbSysEx::encodeCapabilities(const CapabilitySnapshot& s) {
    const InstrumentCapabilities& c = s.capabilities;
    std::vector<uint8_t> m;
    header(m, SysExBlock::Capabilities, SysExDirection::Response);
    m.push_back(b7(c.version));
    m.push_back(b7(c.channel));
    m.push_back(b7(c.gmProgram));
    m.push_back(b7(c.typeId));
    m.push_back(b7(c.subType));
    m.push_back(b7(c.noteMode));
    m.push_back(b7(c.noteMin));
    m.push_back(b7(c.noteMax));
    m.push_back(b7(c.polyphony));
    m.push_back(b7(static_cast<int>(c.discreteNotes.size())));
    for (uint8_t n : c.discreteNotes) m.push_back(b7(n));
    m.push_back(b7(static_cast<int>(c.supportedCc.size())));
    for (uint8_t cc : c.supportedCc) m.push_back(b7(cc));
    m.push_back(b7(static_cast<int>(c.name.size())));
    for (char ch : c.name) m.push_back(b7(static_cast<uint8_t>(ch)));
    m.push_back(kEnd);
    return m;
}

std::vector<uint8_t> GmbSysEx::encodeStringConfigV1(const CapabilitySnapshot& s) {
    const StringInstrumentConfig& c = s.stringConfig;
    std::vector<uint8_t> m;
    header(m, SysExBlock::StringConfig, SysExDirection::Response);
    m.push_back(0x01);  // block version
    m.push_back(b7(c.channel));
    m.push_back(b7(c.stringCount));
    m.push_back(b7(c.fretCount));
    m.push_back(b7(c.isFretless));
    m.push_back(b7(c.capo));
    m.push_back(b7(c.ccActive));
    m.push_back(b7(c.ccString));
    m.push_back(b7(c.ccFret));
    for (uint8_t note : c.tuning) m.push_back(b7(note));
    m.push_back(kEnd);
    return m;
}

std::vector<uint8_t> GmbSysEx::encodeStringConfigV2(const CapabilitySnapshot& s) {
    const StringInstrumentConfig& c = s.stringConfig;
    std::vector<uint8_t> m;
    header(m, SysExBlock::StringConfig, SysExDirection::Response);
    m.push_back(0x02);  // block version 2
    m.push_back(b7(c.channel));
    m.push_back(b7(c.stringCount));
    m.push_back(b7(c.fretCount));
    m.push_back(b7(c.isFretless));
    m.push_back(b7(c.capo));
    m.push_back(b7(c.ccActive));
    m.push_back(b7(c.ccString));
    m.push_back(b7(c.ccFret));
    m.push_back(b7(c.ccStringMin));
    m.push_back(b7(c.ccStringMax));
    m.push_back(b7(c.ccStringOffset + 64));  // signed offset encoding (spec 10.4)
    m.push_back(b7(c.ccFretMin));
    m.push_back(b7(c.ccFretMax));
    m.push_back(b7(c.ccFretOffset + 64));
    m.push_back(b7(c.selectionMode));
    m.push_back(b7(c.stringOrder));
    for (uint8_t note : c.tuning) m.push_back(b7(note));
    // Frets per string (default to global fret count if absent).
    for (int i = 0; i < c.stringCount; ++i) {
        uint8_t f = i < static_cast<int>(c.fretsPerString.size())
                        ? c.fretsPerString[i]
                        : c.fretCount;
        m.push_back(b7(f));
    }
    // String mapping (default identity).
    for (int i = 0; i < c.stringCount; ++i) {
        uint8_t v = i < static_cast<int>(c.mapping.size())
                        ? c.mapping[i]
                        : static_cast<uint8_t>(i);
        m.push_back(b7(v));
    }
    m.push_back(kEnd);
    return m;
}

std::vector<uint8_t> GmbSysEx::encodeNotification(const CapabilitySnapshot& s,
                                                  uint8_t flags) {
    std::vector<uint8_t> m;
    header(m, SysExBlock::Notification, SysExDirection::Notification);
    m.push_back(0x01);  // block version
    m.push_back(b7(s.stringConfig.channel));
    pushRevision(m, s.revision);
    m.push_back(b7(flags));
    m.push_back(kEnd);
    return m;
}

bool GmbSysEx::isWellFormed(const uint8_t* data, size_t len) {
    if (data == nullptr || len < 6 || len > kMaxMessage) return false;
    if (data[0] != kStart || data[len - 1] != kEnd) return false;
    if (data[1] != kManufacturer || data[2] != kGmbId) return false;
    // All interior bytes must be 7-bit.
    for (size_t i = 1; i < len - 1; ++i) {
        if (data[i] & 0x80) return false;
    }
    return true;
}

SysExRequest GmbSysEx::parseRequest(const uint8_t* data, size_t len) {
    SysExRequest r;
    if (!isWellFormed(data, len)) return r;
    if (len < 6) return r;
    r.block = data[3];
    r.direction = data[4];
    if (r.direction != static_cast<uint8_t>(SysExDirection::Request)) {
        // Only requests are parsed here; ignore responses/notifications.
        return r;
    }
    // Blocks 6 and 7 requests carry a channel byte before F7.
    if (r.block == static_cast<uint8_t>(SysExBlock::Capabilities) ||
        r.block == static_cast<uint8_t>(SysExBlock::StringConfig)) {
        if (len < 7) return r;  // channel byte required
        r.hasChannel = true;
        r.channel = data[5];
    }
    r.valid = true;
    return r;
}

std::vector<uint8_t> GmbSysEx::respond(const SysExRequest& req,
                                       const CapabilitySnapshot& snap, bool useV2) {
    if (!req.valid) return {};
    switch (static_cast<SysExBlock>(req.block)) {
        case SysExBlock::Identity:
            return encodeIdentity(snap);
        case SysExBlock::Descriptor:
            return encodeDescriptor(snap);
        case SysExBlock::Capabilities:
            return encodeCapabilities(snap);
        case SysExBlock::StringConfig:
            return useV2 ? encodeStringConfigV2(snap) : encodeStringConfigV1(snap);
        default:
            return {};  // unknown block: ignored (spec section 20)
    }
}

}  // namespace gmb
