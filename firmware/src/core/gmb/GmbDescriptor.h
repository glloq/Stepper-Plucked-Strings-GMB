// GMB v2 instrument descriptor (docs/SYSEX_IDENTITY.md §5) serialiser.
//
// The current General-Midi-Boop controller discovers capabilities from a JSON
// "descriptor" (level 1), not the fixed capability/string-config blocks. This
// renders one CapabilitySnapshot as that descriptor. The document is restricted
// to 7-bit ASCII (non-ASCII escaped as \uXXXX) so it is safe to carry verbatim
// over the block 0x10 SysEx transfer AND over HTTP (GET /gmb/descriptor.json).
//
// Pure core (no ArduinoJson): the snapshot is the single source of truth and the
// output is host-testable without the platform layer.
#pragma once

#include <string>

#include "Capabilities.h"

namespace gmb {

class GmbDescriptor {
public:
    // Serialise `s` as a GMB v2 descriptor JSON document (single instrument,
    // family "strings"). Always 7-bit ASCII.
    static std::string toJson(const CapabilitySnapshot& s);
};

}  // namespace gmb
