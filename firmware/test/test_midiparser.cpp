#include "TestFramework.h"
#include "../src/core/midi/MidiParser.h"

using namespace gmb;

TEST(parser_note_on_off) {
    MidiParser p;
    uint8_t bytes[] = {0x90, 60, 100, 0x80, 60, 0};
    p.feed(bytes, sizeof(bytes), 0);
    CHECK_EQ((int)p.events().size(), 2);
    CHECK(p.events()[0].isNoteOn());
    CHECK_EQ((int)p.events()[0].data1, 60);
    CHECK(p.events()[1].isNoteOff());
}

TEST(parser_running_status) {
    MidiParser p;
    // Note On status once, then three note pairs under running status.
    uint8_t bytes[] = {0x90, 60, 100, 62, 100, 64, 100};
    p.feed(bytes, sizeof(bytes), 0);
    CHECK_EQ((int)p.events().size(), 3);
    CHECK_EQ((int)p.events()[2].data1, 64);
}

// resetStream() drops running status: data bytes fed after a reset with no fresh
// status byte are ignored, so one UDP sender can't ride another's running status.
TEST(parser_reset_stream_drops_running_status) {
    MidiParser p;
    uint8_t first[] = {0x90, 60, 100};  // sender A: Note On, establishes status
    p.feed(first, sizeof(first), 0);
    CHECK_EQ((int)p.events().size(), 1);
    p.clear();
    p.resetStream();                    // datagram boundary (different sender)
    uint8_t second[] = {62, 100};       // sender B: bare data bytes, no status
    p.feed(second, sizeof(second), 0);
    CHECK_EQ((int)p.events().size(), 0);  // must NOT decode under A's old status
}

// resetStream() also abandons an in-progress SysEx so a stray F7 from another
// datagram can't terminate a SysEx started in a previous one.
TEST(parser_reset_stream_abandons_sysex) {
    MidiParser p;
    uint8_t start[] = {0xF0, 0x7D, 0x01};  // sender A: SysEx start, unterminated
    p.feed(start, sizeof(start), 0);
    CHECK_EQ((int)p.sysex().size(), 0);    // still open
    p.resetStream();                        // datagram boundary
    uint8_t end[] = {0x02, 0xF7};           // sender B: data + EOX
    p.feed(end, sizeof(end), 0);
    CHECK_EQ((int)p.sysex().size(), 0);     // no SysEx completed from mixed senders
}

TEST(parser_control_change) {
    MidiParser p;
    uint8_t bytes[] = {0xB2, 20, 3};  // CC20=3 on channel 2
    p.feed(bytes, sizeof(bytes), 0);
    CHECK_EQ((int)p.events().size(), 1);
    CHECK(p.events()[0].isControlChange());
    CHECK_EQ((int)p.events()[0].channel, 2);
    CHECK_EQ((int)p.events()[0].data1, 20);
    CHECK_EQ((int)p.events()[0].data2, 3);
}

TEST(parser_program_change_one_data_byte) {
    MidiParser p;
    uint8_t bytes[] = {0xC0, 24, 0x90, 60, 100};
    p.feed(bytes, sizeof(bytes), 0);
    CHECK_EQ((int)p.events().size(), 2);
    CHECK_EQ((int)p.events()[0].type, 0xC0);
    CHECK(p.events()[1].isNoteOn());
}

TEST(parser_sysex_buffer) {
    MidiParser p;
    uint8_t bytes[] = {0xF0, 0x7D, 0x00, 0x06, 0x01, 0x2A, 0xF7};
    p.feed(bytes, sizeof(bytes), 0);
    CHECK_EQ((int)p.sysex().size(), 1);
    CHECK_EQ((int)p.sysex()[0].front(), 0xF0);
    CHECK_EQ((int)p.sysex()[0].back(), 0xF7);
    CHECK_EQ((int)p.sysex()[0].size(), 7);
}
