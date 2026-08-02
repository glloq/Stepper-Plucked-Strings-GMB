#include "TestFramework.h"
#include "../src/core/instrument/NoteAllocator.h"

using namespace gmb;

static std::vector<StringSpec> guitar6() {
    return {
        {40, 12, true, false}, {45, 12, true, false}, {50, 12, true, false},
        {55, 12, true, false}, {59, 12, true, false}, {64, 12, true, false},
    };  // E2 A2 D3 G3 B3 E4
}

TEST(allocate_single_note_prefers_capable_string) {
    NoteAllocator a;
    a.setStrings(guitar6());
    uint8_t fret = 0;
    int8_t s = a.allocateOne(52, &fret);  // 52 playable on E2(+12), A2(+7), D3(+2)
    CHECK(s >= 0);
    CHECK(a.canPlay(s, 52));
    CHECK_EQ((int)fret, a.fretFor(s, 52));
}

TEST(allocate_open_note) {
    NoteAllocator a;
    a.setStrings(guitar6());
    uint8_t fret = 9;
    int8_t s = a.allocateOne(40, &fret);  // open low E
    CHECK_EQ((int)s, 0);
    CHECK_EQ((int)fret, 0);
}

// Acceptance criterion 14: six axes commanded simultaneously (a 6-note chord).
TEST(allocate_full_chord) {
    NoteAllocator a;
    a.setStrings(guitar6());
    std::vector<uint8_t> chord = {40, 45, 50, 55, 59, 64};
    auto res = a.allocateChord(chord);
    int assigned = 0;
    for (auto& r : res)
        if (r.assigned) ++assigned;
    CHECK_EQ(assigned, 6);
    // Each string used at most once.
    std::vector<int> used(6, 0);
    for (auto& r : res)
        if (r.assigned) used[r.stringIndex]++;
    for (int u : used) CHECK_EQ(u, 1);
}

TEST(saturation_monophonic) {
    NoteAllocator a;
    a.setStrings(guitar6());
    a.setStrategy(SaturationStrategy::Monophonic);
    auto res = a.allocateChord({40, 45, 50});
    int assigned = 0;
    for (auto& r : res)
        if (r.assigned) ++assigned;
    CHECK_EQ(assigned, 1);
}

TEST(faulted_string_not_used) {
    auto specs = guitar6();
    specs[0].faulted = true;  // low E axis faulted
    NoteAllocator a;
    a.setStrings(specs);
    CHECK(!a.canPlay(0, 40));
    // 40 is only the open low-E; with that axis faulted it cannot be played.
    uint8_t fret = 0;
    int8_t s = a.allocateOne(40, &fret);
    CHECK_EQ((int)s, -1);
}
