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
