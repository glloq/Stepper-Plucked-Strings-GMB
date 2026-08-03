#include "NoteAllocator.h"

#include <algorithm>

namespace gmb {

void NoteAllocator::setStrings(const std::vector<StringSpec>& specs) {
    specs_ = specs;
    runtime_.assign(specs.size(), StringRuntime{});
}

bool NoteAllocator::canPlay(int stringIndex, uint8_t note) const {
    if (stringIndex < 0 || stringIndex >= static_cast<int>(specs_.size())) return false;
    const StringSpec& s = specs_[stringIndex];
    if (!s.enabled || s.faulted) return false;
    int n = static_cast<int>(note);
    return n >= s.openNote && n <= s.openNote + s.maxFret;
}

int NoteAllocator::fretFor(int stringIndex, uint8_t note) const {
    if (!canPlay(stringIndex, note)) return -1;
    return static_cast<int>(note) - specs_[stringIndex].openNote;
}

int NoteAllocator::cost(int stringIndex, uint8_t note) const {
    int fret = fretFor(stringIndex, note);
    if (fret < 0) return 1 << 20;
    // Prefer the smallest carriage movement; a tiny bias keeps a well-positioned
    // finger where it is (spec 17.2, priorities 4 & 5).
    return std::abs(fret - runtime_[stringIndex].currentFret);
}

int8_t NoteAllocator::allocateOne(uint8_t note, uint8_t* fretOut) {
    int best = -1;
    int bestCost = 1 << 20;
    for (int i = 0; i < static_cast<int>(specs_.size()); ++i) {
        if (!runtime_[i].free) continue;
        if (!canPlay(i, note)) continue;
        int c = cost(i, note);
        if (c < bestCost) {
            bestCost = c;
            best = i;
        }
    }
    if (best < 0) return -1;
    int fret = fretFor(best, note);
    runtime_[best].free = false;
    runtime_[best].currentFret = fret;
    runtime_[best].noteAge = ageCounter_++;
    if (fretOut) *fretOut = static_cast<uint8_t>(fret);
    return static_cast<int8_t>(best);
}

void NoteAllocator::release(int stringIndex) {
    if (stringIndex >= 0 && stringIndex < static_cast<int>(runtime_.size())) {
        runtime_[stringIndex].free = true;
    }
}

std::vector<Allocation> NoteAllocator::allocateChord(const std::vector<uint8_t>& notes) {
    std::vector<Allocation> result(notes.size());
    for (size_t i = 0; i < notes.size(); ++i) result[i].index = static_cast<int>(i);

    // Order in which we try to satisfy notes, per saturation strategy.
    std::vector<int> order(notes.size());
    for (size_t i = 0; i < notes.size(); ++i) order[i] = static_cast<int>(i);

    switch (strategy_) {
        case SaturationStrategy::PriorityLow:
            std::stable_sort(order.begin(), order.end(),
                             [&](int a, int b) { return notes[a] < notes[b]; });
            break;
        case SaturationStrategy::PriorityHigh:
            std::stable_sort(order.begin(), order.end(),
                             [&](int a, int b) { return notes[a] > notes[b]; });
            break;
        case SaturationStrategy::Monophonic:
            order.resize(std::min<size_t>(1, notes.size()));
            break;
        case SaturationStrategy::PriorityFirst:
        case SaturationStrategy::IgnoreExtra:
        case SaturationStrategy::ReplaceOldest:
            // keep received order
            break;
    }

    // Greedy assignment: constrained notes (fewest capable strings) first so we
    // maximise the number of notes played (priority 1).
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        auto capable = [&](int note) {
            int c = 0;
            for (int s = 0; s < static_cast<int>(specs_.size()); ++s)
                if (canPlay(s, static_cast<uint8_t>(note))) ++c;
            return c;
        };
        return capable(notes[a]) < capable(notes[b]);
    });

    for (int idx : order) {
        uint8_t fret = 0;
        int8_t s = allocateOne(notes[idx], &fret);
        if (s < 0 && strategy_ == SaturationStrategy::ReplaceOldest) {
            // Steal the string holding the oldest note that can play this one.
            int victim = -1;
            uint32_t oldest = UINT32_MAX;
            for (int i = 0; i < static_cast<int>(specs_.size()); ++i) {
                if (canPlay(i, notes[idx]) && !runtime_[i].free &&
                    runtime_[i].noteAge < oldest) {
                    oldest = runtime_[i].noteAge;
                    victim = i;
                }
            }
            if (victim >= 0) {
                runtime_[victim].free = true;
                s = allocateOne(notes[idx], &fret);
            }
        }
        if (s >= 0) {
            result[idx].assigned = true;
            result[idx].stringIndex = s;
            result[idx].fret = fret;
        }
    }
    return result;
}

}  // namespace gmb
