#include "Profile.h"

namespace gmb {

InstrumentView Profile::instrumentView() const {
    InstrumentView v;
    v.stringCount = static_cast<uint8_t>(strings.size());
    v.capo = instrument.capo;
    v.transpose = instrument.transpose + midi.transpose;
    for (const auto& s : strings) {
        v.openNotes.push_back(s.openNote);
        v.maxFretPerString.push_back(s.maxFret);
    }
    return v;
}

Profile Profile::makeDefault(const std::string& name, uint8_t stringCount,
                             const std::vector<uint8_t>& tuning, uint8_t maxFret) {
    Profile p;
    p.instrument.name = name;
    p.instrument.stringCount = stringCount;

    for (uint8_t i = 0; i < stringCount; ++i) {
        AxisConfig axis;
        axis.openNote = i < tuning.size() ? tuning[i] : 40;
        axis.maxFret = maxFret;
        p.strings.push_back(axis);

        HomingConfig h;
        p.homing.push_back(h);

        ServoConfig finger;
        finger.enabled = true;
        finger.function = "finger";
        finger.stringIndex = static_cast<int8_t>(i);
        finger.source = ServoSource::Pca;
        finger.channel = i;             // channels 0..5 : finger press
        p.servos.push_back(finger);

        ServoConfig pluck;
        pluck.enabled = true;
        pluck.function = "pluck";
        pluck.stringIndex = static_cast<int8_t>(i);
        pluck.source = ServoSource::Pca;
        pluck.channel = static_cast<uint8_t>(6 + i);  // channels 6..11 : pluck
        p.servos.push_back(pluck);
    }

    // Default explicit selection follows the General-Midi-Boop preset.
    p.selector.string.maximum = stringCount;
    p.selector.fret.maximum = maxFret;

    // Automatic pin assignment for the reference board.
    if (const BoardProfile* board = builtinBoardProfile(p.boardIdentifier)) {
        PinManager pm(*board);
        PinRequest req;
        req.stringCount = stringCount;
        pm.autoAssign(req);
        p.pins = pm.assignments();
    }
    return p;
}

}  // namespace gmb
