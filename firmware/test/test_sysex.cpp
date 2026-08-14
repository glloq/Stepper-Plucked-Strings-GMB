#include <algorithm>

#include "TestFramework.h"
#include "../src/core/configuration/Profile.h"
#include "../src/core/gmb/Capabilities.h"
#include "../src/core/gmb/GmbDescriptor.h"
#include "../src/core/gmb/GmbSysEx.h"
#include "../src/core/gmb/GmbSysExService.h"

using namespace gmb;

static Profile guitarProfile() {
    return Profile::makeDefault("Guitar", 6, {40, 45, 50, 55, 59, 64}, 12);
}

// Acceptance criteria 5, 6, 7 (SysEx spec 24): range, polyphony, CCs.
TEST(snapshot_range_polyphony_and_cc) {
    Profile p = guitarProfile();
    CapabilitySnapshot s = buildSnapshot(p);
    CHECK(s.valid);
    // Standard guitar has a continuous playable range 40..76.
    CHECK_EQ((int)s.capabilities.noteMode, 0);
    CHECK_EQ((int)s.capabilities.noteMin, 40);
    CHECK_EQ((int)s.capabilities.noteMax, 76);
    // Polyphony = active strings.
    CHECK_EQ((int)s.capabilities.polyphony, 6);
    // Announced CCs include the configured selectors (20/21) and standards.
    auto has = [&](uint8_t cc) {
        for (uint8_t x : s.capabilities.supportedCc)
            if (x == cc) return true;
        return false;
    };
    CHECK(has(7));
    CHECK(has(11));
    CHECK(has(20));
    CHECK(has(21));
    CHECK(has(64));
    CHECK(has(120));
    CHECK(has(123));
}

// Discrete-note mode when a gap exists (SysEx spec 5.2).
TEST(snapshot_discrete_notes_when_gap) {
    // Two strings far apart with 0 frets: only two isolated notes.
    Profile p = Profile::makeDefault("Sparse", 2, {40, 80}, 0);
    CapabilitySnapshot s = buildSnapshot(p);
    CHECK_EQ((int)s.capabilities.noteMode, 1);
    CHECK_EQ((int)s.capabilities.discreteNotes.size(), 2);
}

// Custom CCs are announced instead of the defaults (SysEx spec section 7).
TEST(snapshot_custom_cc_announced) {
    Profile p = guitarProfile();
    p.selector.string.ccNumber = 24;
    p.selector.fret.ccNumber = 25;
    CapabilitySnapshot s = buildSnapshot(p);
    auto has = [&](uint8_t cc) {
        for (uint8_t x : s.capabilities.supportedCc)
            if (x == cc) return true;
        return false;
    };
    CHECK(has(24));
    CHECK(has(25));
    CHECK(!has(20));
    CHECK(!has(21));
}

// Block 1 identity encoding shape (SysEx spec 4.1).
TEST(block1_identity_encoding) {
    Profile p = guitarProfile();
    CapabilitySnapshot s = buildSnapshot(p);
    auto m = GmbSysEx::encodeIdentity(s);
    CHECK(GmbSysEx::isWellFormed(m.data(), m.size()));
    CHECK_EQ((int)m[0], 0xF0);
    CHECK_EQ((int)m[1], 0x7D);
    CHECK_EQ((int)m[2], 0x00);
    CHECK_EQ((int)m[3], 0x01);  // block 1
    CHECK_EQ((int)m[4], 0x01);  // response
    CHECK_EQ((int)m.back(), 0xF7);
    // 5 header + version + 5 id + 32 name + 3 fw + 5 features + end = 52.
    CHECK_EQ((int)m.size(), 52);
}

// Block 6 request -> response round trip; all bytes 7-bit (SysEx spec 4.3).
TEST(block6_request_response) {
    Profile p = guitarProfile();
    CapabilitySnapshot s = buildSnapshot(p);
    uint8_t req[] = {0xF0, 0x7D, 0x00, 0x06, 0x00, 0x00, 0xF7};
    SysExRequest r = GmbSysEx::parseRequest(req, sizeof(req));
    CHECK(r.valid);
    CHECK_EQ((int)r.block, 6);
    CHECK(r.hasChannel);
    auto resp = GmbSysEx::respond(r, s);
    CHECK(GmbSysEx::isWellFormed(resp.data(), resp.size()));
    CHECK_EQ((int)resp[3], 0x06);
}

// Block 7 v1 tuning is announced in PHYSICAL string order (this profile is
// already ascending, so positional == low->high here). The ordering is pinned
// more strictly by tuning_is_positional_not_sorted below.
TEST(block7_tuning_low_to_high) {
    Profile p = guitarProfile();
    CapabilitySnapshot s = buildSnapshot(p);
    auto m = GmbSysEx::encodeStringConfigV1(s);
    CHECK(GmbSysEx::isWellFormed(m.data(), m.size()));
    // Layout: F0 7D 00 07 01 | ver ch nStr nFret fretless capo ccAct ccStr ccFret | tuning...
    size_t tuningStart = 5 + 9;
    CHECK_EQ((int)m[tuningStart + 0], 40);
    CHECK_EQ((int)m[tuningStart + 5], 64);
    CHECK_EQ((int)m[5 + 2], 6);   // string count
    CHECK_EQ((int)m[5 + 7], 20);  // ccString
    CHECK_EQ((int)m[5 + 8], 21);  // ccFret
}

// Block 7 v2 encodes signed offsets as offset+64 (SysEx spec 10.4).
TEST(block7_v2_signed_offset) {
    Profile p = guitarProfile();
    p.selector.string.offset = -3;
    p.selector.fret.offset = 5;
    CapabilitySnapshot s = buildSnapshot(p);
    auto m = GmbSysEx::encodeStringConfigV2(s);
    CHECK(GmbSysEx::isWellFormed(m.data(), m.size()));
    CHECK_EQ((int)m[5], 0x02);  // version 2
    // ...channel,nStr,nFret,fretless,capo,ccAct,ccStr,ccFret (8) then
    // ccStrMin,ccStrMax,ccStrOff(+64)
    size_t base = 5 + 1 + 8;
    CHECK_EQ((int)m[base + 2], 64 - 3);  // string offset -3 -> 61
    CHECK_EQ((int)m[base + 5], 64 + 5);  // fret offset +5 -> 69
}

// Block 8 notification carries a decodable revision (SysEx spec section 11/13).
TEST(block8_notification_revision) {
    Profile p = guitarProfile();
    p.capabilitiesRevision = 300;  // > 127, needs multi-byte encoding
    CapabilitySnapshot s = buildSnapshot(p);
    auto m = GmbSysEx::encodeNotification(s, kStringConfigChanged | kCcMappingChanged);
    CHECK(GmbSysEx::isWellFormed(m.data(), m.size()));
    CHECK_EQ((int)m[3], 0x08);  // block 8
    CHECK_EQ((int)m[4], 0x02);  // spontaneous notification
    // Decode 5x7-bit revision (big-endian) starting after ver+channel.
    size_t revStart = 5 + 1 + 1;
    uint32_t rev = 0;
    for (int i = 0; i < 5; ++i) rev = (rev << 7) | m[revStart + i];
    CHECK_EQ((int)rev, 300);
}

// Robustness: malformed / non-7-bit messages are rejected (SysEx spec 20).
TEST(sysex_rejects_malformed) {
    uint8_t noStart[] = {0x00, 0x7D, 0x00, 0x06, 0x00, 0xF7};
    CHECK(!GmbSysEx::isWellFormed(noStart, sizeof(noStart)));
    uint8_t highBit[] = {0xF0, 0x7D, 0x00, 0x06, 0x00, 0x80, 0xF7};  // 0x80 invalid
    CHECK(!GmbSysEx::isWellFormed(highBit, sizeof(highBit)));
    uint8_t wrongMfr[] = {0xF0, 0x7E, 0x00, 0x06, 0x00, 0x00, 0xF7};
    CHECK(!GmbSysEx::isWellFormed(wrongMfr, sizeof(wrongMfr)));
    // Unknown block: parseRequest returns invalid.
    uint8_t unknown[] = {0xF0, 0x7D, 0x00, 0x63, 0x00, 0xF7};
    SysExRequest r = GmbSysEx::parseRequest(unknown, sizeof(unknown));
    auto resp = GmbSysEx::respond(r, buildSnapshot(guitarProfile()));
    CHECK(resp.empty());
}

// The announced tuning is in PHYSICAL string order and stays index-aligned with
// fretsPerString. A re-entrant ukulele (G4 C4 E4 A4) is the case that catches a
// sort: string 1 is the HIGHEST-pitched one, and pairing its open note with
// another string's fret count would misdescribe every voice.
TEST(tuning_is_positional_not_sorted) {
    Profile p = Profile::makeDefault("Uke", 4, {67, 60, 64, 69}, 12);
    p.strings[0].maxFret = 5;   // give the strings distinguishable reaches
    p.strings[3].maxFret = 15;
    CapabilitySnapshot s = buildSnapshot(p);
    CHECK_EQ((int)s.stringConfig.tuning.size(), 4);
    CHECK_EQ((int)s.stringConfig.tuning[0], 67);  // physical order, NOT sorted
    CHECK_EQ((int)s.stringConfig.tuning[1], 60);
    CHECK_EQ((int)s.stringConfig.tuning[2], 64);
    CHECK_EQ((int)s.stringConfig.tuning[3], 69);
    CHECK_EQ((int)s.stringConfig.fretsPerString.size(), 4);
    CHECK_EQ((int)s.stringConfig.fretsPerString[0], 5);   // aligned with tuning[0]
    CHECK_EQ((int)s.stringConfig.fretsPerString[3], 15);
}

// The announced tuning folds in the global transpose, so tuning + capo reproduces
// the announced playable range (which already includes both).
TEST(tuning_folds_transpose_so_range_reproduces) {
    Profile p = Profile::makeDefault("Uke", 4, {67, 60, 64, 69}, 12);
    p.instrument.transpose = 2;
    CapabilitySnapshot s = buildSnapshot(p);
    CHECK_EQ((int)s.stringConfig.tuning[0], 69);  // 67 + 2
    CHECK_EQ((int)s.stringConfig.tuning[1], 62);  // 60 + 2
    // The lowest announced note is the lowest announced open string (+ capo 0).
    int lowest = 127;
    for (uint8_t t : s.stringConfig.tuning) lowest = std::min(lowest, (int)t);
    CHECK_EQ((int)s.capabilities.noteMin, lowest + s.stringConfig.capo);
}

// ============================ GMB v2 (descriptor) ============================
//
// The current General-Midi-Boop controller discovers an instrument through the v2
// handshake and a JSON descriptor, not the fixed v1 capability blocks. These
// tests pin the wire shapes; the descriptor content itself is derived entirely
// from CapabilitySnapshot, so it stays correct for the stepper build without any
// mechanism-specific handling.

TEST(v2_handshake_shape) {
    CapabilitySnapshot s = buildSnapshot(guitarProfile());
    s.revision = 300;
    auto m = GmbSysEx::encodeHandshakeV2(s, 512, 0x03);
    CHECK(GmbSysEx::isWellFormed(m.data(), m.size()));
    CHECK_EQ((int)m.size(), 24);
    CHECK_EQ((int)m[3], 0x01);  // block 1
    CHECK_EQ((int)m[4], 0x01);  // response
    CHECK_EQ((int)m[5], 0x02);  // proto_ver v2
    CHECK_EQ((int)m.back(), 0xF7);
    int descSize = m[14] | (m[15] << 7) | (m[16] << 14);
    CHECK_EQ(descSize, 512);
    uint32_t rev = m[17] | (m[18] << 7) | (m[19] << 14) | ((uint32_t)m[20] << 21) |
                   ((uint32_t)(m[21] & 0x0F) << 28);
    CHECK_EQ((int)rev, 300);
    CHECK_EQ((int)m[22], 0x03);  // flags
}

// The descriptor JSON carries the v2 core fields, is 7-bit clean (it travels over
// SysEx verbatim), and reassembles from its 0x10 chunks byte for byte.
TEST(v2_descriptor_json_and_chunking) {
    CapabilitySnapshot s = buildSnapshot(guitarProfile());
    std::string j = GmbDescriptor::toJson(s);
    CHECK(j.find("\"gmb_descriptor\":2") != std::string::npos);
    CHECK(j.find("\"family\":\"strings\"") != std::string::npos);
    CHECK(j.find("\"string_count\":6") != std::string::npos);
    CHECK(j.find("\"polyphony\":{\"max\":6") != std::string::npos);
    for (char c : j) CHECK((static_cast<unsigned char>(c) & 0x80) == 0);  // 7-bit clean

    size_t total = (j.size() + 199) / 200;
    std::string re;
    for (uint16_t idx = 0; idx < total; ++idx) {
        auto ch = GmbSysEx::encodeDescriptorChunk(j, idx);
        CHECK(GmbSysEx::isWellFormed(ch.data(), ch.size()));
        int t = ch[5] | (ch[6] << 7);
        CHECK_EQ(t, (int)total);
        int i = ch[7] | (ch[8] << 7);
        CHECK_EQ(i, (int)idx);
        for (size_t k = 9; k + 1 < ch.size(); ++k) re += (char)ch[k];  // payload -> F7
    }
    CHECK(re == j);
}

// One voice per physical carriage, each a single-note range from its open note to
// its highest reachable fret. On this machine that IS the polyphony limit: a
// carriage plays one note at a time.
TEST(v2_descriptor_one_voice_per_carriage) {
    CapabilitySnapshot s = buildSnapshot(guitarProfile());
    std::string j = GmbDescriptor::toJson(s);
    CHECK(j.find("\"constraints\":[{\"type\":\"one_note_per_voice\"}]") != std::string::npos);
    CHECK(j.find("{\"id\":\"s1\",\"notes\":{\"mode\":\"range\",\"min\":40,\"max\":52}}") != std::string::npos);
    CHECK(j.find("{\"id\":\"s6\",\"notes\":{\"mode\":\"range\",\"min\":64,\"max\":76}}") != std::string::npos);
    size_t voices = 0;
    for (size_t i = j.find("\"voices\":["); i != std::string::npos && i < j.size(); ) {
        i = j.find("{\"id\":\"s", i + 1);
        if (i == std::string::npos) break;
        ++voices;
    }
    CHECK_EQ((int)voices, 6);
}

// The v2 change notification is a 12-byte 0x11 frame with a little-endian revision.
TEST(v2_change_notification) {
    CapabilitySnapshot s = buildSnapshot(guitarProfile());
    s.revision = 300;
    auto m = GmbSysEx::encodeChangeNotification(s, 0x02);
    CHECK(GmbSysEx::isWellFormed(m.data(), m.size()));
    CHECK_EQ((int)m.size(), 12);
    CHECK_EQ((int)m[3], 0x11);
    CHECK_EQ((int)m[4], 0x02);
    uint32_t rev = m[5] | (m[6] << 7) | (m[7] << 14) | ((uint32_t)m[8] << 21) |
                   ((uint32_t)(m[9] & 0x0F) << 28);
    CHECK_EQ((int)rev, 300);
    CHECK_EQ((int)m[10], 0x02);
}

// End to end through the service: a Block-1 request returns the v2 handshake
// (level 1, descriptor_size > 0), and a 0x10 request returns a descriptor segment.
TEST(v2_service_discovery) {
    GmbSysExService svc;
    svc.rebuild(guitarProfile());

    uint8_t id[] = {0xF0, 0x7D, 0x00, 0x01, 0x00, 0xF7};
    auto hs = svc.handleMessage(id, sizeof(id), 1);
    CHECK_EQ((int)hs.size(), 24);
    CHECK_EQ((int)hs[5], 0x02);
    int descSize = hs[14] | (hs[15] << 7) | (hs[16] << 14);
    CHECK(descSize > 0);  // level 1: a descriptor is available
    CHECK_EQ((size_t)descSize, svc.descriptorJson().size());

    uint8_t chunk[] = {0xF0, 0x7D, 0x00, 0x10, 0x00, 0x00, 0x00, 0xF7};
    auto seg = svc.handleMessage(chunk, sizeof(chunk), 2);
    CHECK(seg.size() > 9);
    CHECK_EQ((int)seg[3], 0x10);
    CHECK_EQ((int)seg[4], 0x01);
}
