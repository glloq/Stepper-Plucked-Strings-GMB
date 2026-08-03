#include "Capabilities.h"

#include <algorithm>
#include <set>

#include "../configuration/Profile.h"

namespace gmb {

CapabilitySnapshot buildSnapshot(const Profile& p, int polyphonyOverride) {
    CapabilitySnapshot snap;
    snap.revision = p.capabilitiesRevision;
    snap.valid = true;

    const uint8_t channel = p.midi.omni ? 0 : p.midi.globalChannel;
    const int capo = p.instrument.capo;
    const int transpose = p.instrument.transpose + p.midi.transpose;

    // ---- Identity ----
    snap.identity.deviceName = p.instrument.name;
    snap.identity.features = 0x38;  // descriptor + capabilities + string config
    snap.identity.firmware[0] = 1;
    snap.identity.firmware[1] = 0;
    snap.identity.firmware[2] = 0;

    // ---- Descriptor ----
    snap.descriptor.channel = channel;
    snap.descriptor.gmProgram = p.instrument.gmProgram;
    snap.descriptor.typeId = p.instrument.typeId;

    // ---- Playable range (spec section 5) ----
    std::set<int> playable;
    int minNote = 128, maxNote = -1;
    uint8_t activeStrings = 0;
    for (const auto& s : p.strings) {
        if (!s.enabled) continue;
        ++activeStrings;
        int lo = s.openNote + capo + transpose;
        int hi = lo + s.maxFret;
        for (int n = lo; n <= hi; ++n) {
            if (n < 0 || n > 127) continue;
            playable.insert(n);
            minNote = std::min(minNote, n);
            maxNote = std::max(maxNote, n);
        }
    }
    if (maxNote < 0) {
        // No enabled/working string: the snapshot is NOT a playable instrument.
        // Mark it invalid instead of announcing a bogus 0..0 range so a host can
        // tell "silent, degraded to nothing" from "can play MIDI note 0".
        minNote = 0;
        maxNote = 0;
        snap.valid = false;
    }

    InstrumentCapabilities& caps = snap.capabilities;
    caps.channel = channel;
    caps.gmProgram = p.instrument.gmProgram;
    caps.typeId = p.instrument.typeId;
    caps.name = p.instrument.name;
    caps.noteMin = static_cast<uint8_t>(minNote);
    caps.noteMax = static_cast<uint8_t>(maxNote);

    // Continuous only if every note in [min,max] is playable somewhere.
    bool continuous = true;
    for (int n = minNote; n <= maxNote; ++n) {
        if (playable.find(n) == playable.end()) {
            continuous = false;
            break;
        }
    }
    if (continuous) {
        caps.noteMode = 0;
    } else {
        caps.noteMode = 1;
        for (int n : playable) caps.discreteNotes.push_back(static_cast<uint8_t>(n));
    }

    // ---- Polyphony (spec section 6) ----
    caps.polyphony = polyphonyOverride >= 0
                         ? static_cast<uint8_t>(polyphonyOverride)
                         : activeStrings;

    // ---- Announced CCs (spec section 7): only activated ones ----
    caps.supportedCc.push_back(7);    // volume
    caps.supportedCc.push_back(11);   // expression
    if (p.selector.enabled) {
        caps.supportedCc.push_back(p.selector.string.ccNumber);
        caps.supportedCc.push_back(p.selector.fret.ccNumber);
    }
    if (p.midi.sustainPedal) caps.supportedCc.push_back(p.midi.sustainCc);
    caps.supportedCc.push_back(120);  // all sound off
    caps.supportedCc.push_back(123);  // all notes off
    std::sort(caps.supportedCc.begin(), caps.supportedCc.end());

    // ---- String configuration (spec section 8 / 10) ----
    StringInstrumentConfig& sc = snap.stringConfig;
    sc.channel = channel;
    // Announce only the strings that are actually available (enabled). In a
    // degraded run the faulted axes are disabled in the runtime profile copy, so
    // the whole string config shrinks consistently.
    uint8_t enabledCount = 0;
    uint8_t maxFret = 0;
    for (const auto& s : p.strings) {
        if (!s.enabled) continue;
        ++enabledCount;
        maxFret = std::max<uint8_t>(maxFret, s.maxFret);
    }
    sc.stringCount = enabledCount;
    sc.fretCount = maxFret;
    sc.isFretless = 0;
    sc.capo = static_cast<uint8_t>(capo);
    sc.ccActive = p.selector.enabled ? 1 : 0;
    sc.ccString = p.selector.string.ccNumber;
    sc.ccFret = p.selector.fret.ccNumber;

    // Tuning announced low -> high (spec section 8).
    std::vector<uint8_t> tuning;
    for (const auto& s : p.strings)
        if (s.enabled) tuning.push_back(s.openNote);
    std::sort(tuning.begin(), tuning.end());
    sc.tuning = tuning;

    // A degraded run announces only the enabled strings (renumbered 1..N), so the
    // CC bounds and the custom mapping must be made consistent with that reduced
    // set — otherwise a client sees, say, 3 strings but a CC range / mapping still
    // referencing the original 4 axes (audit P1-6).
    const bool degraded = enabledCount < p.strings.size();

    // v2 extras.
    sc.ccStringMin = p.selector.string.minimum;
    sc.ccStringMax = p.selector.string.maximum;
    sc.ccStringOffset = p.selector.string.offset;
    sc.ccFretMin = p.selector.fret.minimum;
    sc.ccFretMax = p.selector.fret.maximum;
    sc.ccFretOffset = p.selector.fret.offset;
    sc.selectionMode = static_cast<uint8_t>(p.selector.mode);
    sc.stringOrder = p.selector.string.reverseOrder ? 1 : 0;
    for (const auto& s : p.strings)
        if (s.enabled) sc.fretsPerString.push_back(s.maxFret);
    if (degraded) {
        // Reduced set: clamp the CC range to what is actually announced and drop
        // the custom mapping (it references the original physical axes and would
        // be incoherent against the renumbered set).
        if (sc.ccStringMax > enabledCount) sc.ccStringMax = enabledCount;
        if (sc.ccStringMin > sc.ccStringMax) sc.ccStringMin = sc.ccStringMax;
    } else {
        if (!p.selector.string.mapping.empty()) sc.stringOrder = 2;
        for (int8_t m : p.selector.string.mapping)
            sc.mapping.push_back(static_cast<uint8_t>(m));
    }

    snap.identity.deviceName = p.instrument.name;
    return snap;
}

}  // namespace gmb
