// General-Midi-Boop SysEx encoder/decoder (spec "Communication ... SysEx").
//
// Header: F0 7D 00 <block> <direction> ... F7
//   7D = experimental/educational manufacturer id, 00 = GMB id.
// The service is transport-independent (spec section 21): it only deals in
// complete MIDI byte buffers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Capabilities.h"

namespace gmb {

enum class SysExBlock : uint8_t {
    Identity = 1,               // request -> v2 handshake (was v1 identity)
    Descriptor = 5,             // deprecated fixed-block descriptor
    Capabilities = 6,           // deprecated fixed-block capabilities
    StringConfig = 7,           // deprecated fixed-block string config
    Notification = 8,           // deprecated v1 change notification
    DescriptorTransfer = 0x10,  // GMB v2 JSON descriptor, segmented transfer
    ChangeNotification = 0x11,  // GMB v2 spontaneous change notification
};

enum class SysExDirection : uint8_t { Request = 0, Response = 1, Notification = 2 };

// Capabilities-changed flags (spec section 11).
enum ChangeFlag : uint8_t {
    kIdentityChanged = 1 << 0,
    kDescriptorChanged = 1 << 1,
    kCapabilitiesChanged = 1 << 2,
    kStringConfigChanged = 1 << 3,
    kCcMappingChanged = 1 << 4,
    kRestartRequired = 1 << 5,
};

struct SysExRequest {
    bool valid = false;
    uint8_t block = 0;
    uint8_t direction = 0;
    bool hasChannel = false;
    uint8_t channel = 0;
    bool hasChunkIndex = false;  // block 0x10 descriptor-transfer request
    uint16_t chunkIndex = 0;
};

class GmbSysEx {
public:
    static constexpr uint8_t kStart = 0xF0;
    static constexpr uint8_t kEnd = 0xF7;
    static constexpr uint8_t kManufacturer = 0x7D;
    static constexpr uint8_t kGmbId = 0x00;
    static constexpr size_t kMaxMessage = 512;
    static constexpr uint8_t kProtoVer = 0x02;              // GMB protocol v2
    static constexpr size_t kDescriptorChunkPayload = 200;  // §3 (keeps frame <=210)

    // ---- GMB v2 (docs/SYSEX_IDENTITY.md) ----
    // 24-byte handshake: the response to a Block-1 request. `descriptorSize` is the
    // byte length of the JSON descriptor (0 = level 0, no descriptor); `flags` bit
    // 0 = HTTP descriptor available, bit 1 = push notifications.
    static std::vector<uint8_t> encodeHandshakeV2(const CapabilitySnapshot& s,
                                                  uint32_t descriptorSize,
                                                  uint8_t flags);
    // One 0x10 descriptor segment: F0 7D 00 10 01 <total[2]> <index[2]> <payload> F7.
    static std::vector<uint8_t> encodeDescriptorChunk(const std::string& json,
                                                      uint16_t index);
    // 0x11 spontaneous change notification: F0 7D 00 11 02 <revision[5]> <flags> F7.
    static std::vector<uint8_t> encodeChangeNotification(const CapabilitySnapshot& s,
                                                         uint8_t flags);

    static std::vector<uint8_t> encodeIdentity(const CapabilitySnapshot& s);
    static std::vector<uint8_t> encodeDescriptor(const CapabilitySnapshot& s);
    static std::vector<uint8_t> encodeCapabilities(const CapabilitySnapshot& s);
    static std::vector<uint8_t> encodeStringConfigV1(const CapabilitySnapshot& s);
    static std::vector<uint8_t> encodeStringConfigV2(const CapabilitySnapshot& s);
    static std::vector<uint8_t> encodeNotification(const CapabilitySnapshot& s,
                                                   uint8_t flags);

    // Header + trailer validity and 7-bit payload check (spec section 20).
    static bool isWellFormed(const uint8_t* data, size_t len);

    // Parse a request message. `valid` is false if it isn't a GMB request.
    static SysExRequest parseRequest(const uint8_t* data, size_t len);

    // Produce the response for a parsed request from a snapshot.
    static std::vector<uint8_t> respond(const SysExRequest& req,
                                        const CapabilitySnapshot& snap,
                                        bool useV2 = false);
};

}  // namespace gmb
