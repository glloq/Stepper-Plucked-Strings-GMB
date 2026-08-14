#include "GmbDescriptor.h"

#include <algorithm>
#include <string>

namespace gmb {

namespace {

void appendHex4(std::string& out, unsigned cp) {
    static const char* kHex = "0123456789abcdef";
    out += "\\u";
    out += kHex[(cp >> 12) & 0xF];
    out += kHex[(cp >> 8) & 0xF];
    out += kHex[(cp >> 4) & 0xF];
    out += kHex[cp & 0xF];
}

// JSON-escape a UTF-8 string into 7-bit ASCII: quote/backslash/control chars are
// escaped, and any non-ASCII code point is emitted as \uXXXX so the descriptor is
// safe to carry over the 7-bit SysEx transfer (docs/SYSEX_IDENTITY.md §3).
std::string esc(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    const size_t n = in.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == '"') { out += "\\\""; ++i; }
        else if (c == '\\') { out += "\\\\"; ++i; }
        else if (c == '\n') { out += "\\n"; ++i; }
        else if (c == '\r') { out += "\\r"; ++i; }
        else if (c == '\t') { out += "\\t"; ++i; }
        else if (c < 0x20) { appendHex4(out, c); ++i; }
        else if (c < 0x80) { out += static_cast<char>(c); ++i; }
        else {
            // Decode one UTF-8 sequence to a code point; emit it as \uXXXX.
            unsigned cp = 0xFFFD;
            size_t adv = 1;
            auto cont = [&](size_t k) { return static_cast<unsigned char>(in[i + k]) & 0x3F; };
            if ((c & 0xE0) == 0xC0 && i + 1 < n) {
                cp = ((c & 0x1F) << 6) | cont(1);
                adv = 2;
            } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
                cp = ((c & 0x0F) << 12) | (cont(1) << 6) | cont(2);
                adv = 3;
            } else if ((c & 0xF8) == 0xF0 && i + 3 < n) {
                cp = 0xFFFD;  // outside the BMP -> replacement char (rare for names)
                adv = 4;
            }
            appendHex4(out, cp);
            i += adv;
        }
    }
    return out;
}

void appendIntArray(std::string& j, const std::vector<uint8_t>& v) {
    j += '[';
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) j += ',';
        j += std::to_string(v[i]);
    }
    j += ']';
}

int clampNote(int n) { return std::max(0, std::min(127, n)); }

}  // namespace

std::string GmbDescriptor::toJson(const CapabilitySnapshot& s) {
    const InstrumentCapabilities& caps = s.capabilities;
    const StringInstrumentConfig& sc = s.stringConfig;

    std::string j;
    j.reserve(640);
    j += "{\"gmb_descriptor\":2";
    j += ",\"revision\":" + std::to_string(s.revision);
    j += ",\"device\":{\"name\":\"" + esc(s.identity.deviceName) + "\"";
    j += ",\"model\":\"" + esc(s.identity.model) + "\"}";

    j += ",\"instruments\":[{";
    j += "\"channel\":" + std::to_string(caps.channel);
    j += ",\"configured\":true";
    j += ",\"name\":\"" + esc(caps.name) + "\"";
    j += ",\"gm_program\":" + std::to_string(caps.gmProgram);
    // `type` is a display/scoring hint; GMB also derives it from gm_program, so an
    // unknown key is harmless. Omitted when empty (absent = unknown, §5.1).
    if (!s.descriptor.type.empty())
        j += ",\"type\":\"" + esc(s.descriptor.type) + "\"";

    // notes: range when contiguous, else the exact discrete list (audit SX-1).
    j += ",\"notes\":";
    if (caps.noteMode == 0) {
        j += "{\"mode\":\"range\",\"min\":" + std::to_string(caps.noteMin) +
             ",\"max\":" + std::to_string(caps.noteMax) + "}";
    } else {
        j += "{\"mode\":\"discrete\",\"list\":";
        appendIntArray(j, caps.discreteNotes);
        j += "}";
    }

    // polyphony: max + the strings constraint (one note per string/voice).
    j += ",\"polyphony\":{\"max\":" + std::to_string(caps.polyphony) +
         ",\"constraints\":[{\"type\":\"one_note_per_voice\"}]}";

    // expression: the announced (enabled) control changes.
    j += ",\"expression\":{\"cc\":";
    appendIntArray(j, caps.supportedCc);
    j += "}";

    // voices: one per physical string, each a single-note range built from the
    // aligned tuning/frets arrays (audit SX-2/SX-3 make these consistent).
    j += ",\"voices\":[";
    for (size_t i = 0; i < sc.tuning.size(); ++i) {
        if (i) j += ',';
        const int lo = clampNote(static_cast<int>(sc.tuning[i]) + sc.capo);
        const int reach = i < sc.fretsPerString.size() ? sc.fretsPerString[i] : sc.fretCount;
        const int hi = clampNote(lo + reach);
        j += "{\"id\":\"s" + std::to_string(i + 1) +
             "\",\"notes\":{\"mode\":\"range\",\"min\":" + std::to_string(lo) +
             ",\"max\":" + std::to_string(hi) + "}}";
    }
    j += "]";

    // physical (family "strings") — replaces the fixed block 7 (§5.9).
    j += ",\"physical\":{\"family\":\"strings\"";
    j += ",\"string_count\":" + std::to_string(sc.stringCount);
    j += ",\"fret_count\":" + std::to_string(sc.fretCount);
    j += ",\"frets_per_string\":";
    appendIntArray(j, sc.fretsPerString);
    j += ",\"fretless\":false";
    j += ",\"capo\":" + std::to_string(sc.capo);
    j += ",\"tuning\":";
    appendIntArray(j, sc.tuning);
    const char* order =
        sc.stringOrder == 1 ? "reversed" : (sc.stringOrder == 2 ? "custom" : "normal");
    j += ",\"string_order\":\"" + std::string(order) + "\"";
    // selection: only when CC selection is actually active (§7: an absent block is
    // "unknown" and GMB keeps its own; announcing it would falsely enable CC).
    if (sc.ccActive) {
        const char* mode =
            sc.selectionMode == 0 ? "automatic" : (sc.selectionMode == 1 ? "explicit" : "hybrid");
        j += ",\"selection\":{\"mode\":\"" + std::string(mode) + "\"";
        j += ",\"cc_string\":" + std::to_string(sc.ccString);
        j += ",\"cc_string_min\":" + std::to_string(sc.ccStringMin);
        j += ",\"cc_string_max\":" + std::to_string(sc.ccStringMax);
        j += ",\"cc_string_offset\":" + std::to_string(static_cast<int>(sc.ccStringOffset));
        j += ",\"cc_fret\":" + std::to_string(sc.ccFret);
        j += ",\"cc_fret_min\":" + std::to_string(sc.ccFretMin);
        j += ",\"cc_fret_max\":" + std::to_string(sc.ccFretMax);
        j += ",\"cc_fret_offset\":" + std::to_string(static_cast<int>(sc.ccFretOffset));
        j += "}";
    }
    j += "}";  // physical

    j += "}]}";  // instrument, instruments, root
    return j;
}

}  // namespace gmb
