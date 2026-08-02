// General-Midi-Boop SysEx encoder/decoder (spec "Communication ... SysEx").
//
// Header: F0 7D 00 <block> <direction> ... F7
//   7D = experimental/educational manufacturer id, 00 = GMB id.
// The service is transport-independent (spec section 21): it only deals in
// complete MIDI byte buffers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Capabilities.h"

namespace gmb {

enum class SysExBlock : uint8_t {
    Identity = 1,
    Descriptor = 5,
    Capabilities = 6,
    StringConfig = 7,
    Notification = 8,
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
};

class GmbSysEx {
public:
    static constexpr uint8_t kStart = 0xF0;
    static constexpr uint8_t kEnd = 0xF7;
    static constexpr uint8_t kManufacturer = 0x7D;
    static constexpr uint8_t kGmbId = 0x00;
    static constexpr size_t kMaxMessage = 512;

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
