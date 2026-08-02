#include "TestFramework.h"
#include "../src/core/configuration/Profile.h"
#include "../src/core/gmb/Capabilities.h"
#include "../src/core/gmb/GmbSysEx.h"

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

// Block 7 v1 tuning is announced low -> high (SysEx spec section 8).
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
