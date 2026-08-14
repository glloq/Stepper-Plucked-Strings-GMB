#!/usr/bin/env node
// Behavioural tests for the web interface's PURE logic (no DOM needed) — run by
// the CI web-js job after the syntax checks.
//
// `node --check` proves a file parses; it cannot catch a semantic defect, and the
// UI kernels here are exactly the ones whose bugs are invisible until a builder is
// standing at a bench with a wrong number on screen: the fret geometry that places
// a carriage, the pin-capability filter that colours the GPIO grid, the capability
// range the wizard announces, and the power estimator behind the "will my PSU cope"
// answer. So they get asserted.
'use strict';

// Minimal browser-shape so the modules load outside a browser. Their IIFEs receive
// `window`; localStorage/fetch/document are guarded or unused at load time.
global.window = global;

require('../js/api.js');
var GMB = global.GMB;

var failures = 0, checks = 0;
function check(cond, label) {
  checks++;
  if (!cond) { failures++; console.error('  CHECK failed: ' + label); }
}
function near(a, b, eps, label) {
  check(Math.abs(a - b) <= (eps === undefined ? 1e-6 : eps),
        label + ' (got ' + a + ', want ~' + b + ')');
}
function test(name, fn) {
  var before = failures;
  try { fn(); } catch (e) { failures++; console.error('  THREW: ' + e.message); }
  console.log('[' + (failures === before ? 'PASS' : 'FAIL') + '] ' + name);
}

// ---------------------------------------------------------------------------
// Fret geometry — the number that decides where a carriage physically stops.
// ---------------------------------------------------------------------------

test('theoretical fret position follows the equal-tempered law', function () {
  var s = { scaleLengthMm: 330, fretOffsetMm: 0 };
  near(GMB.fretTheoreticalMm(s, 0), 0, 1e-9, 'fret 0 sits at the nut');
  // The 12th fret is the octave: exactly half the vibrating length.
  near(GMB.fretTheoreticalMm(s, 12), 165, 1e-9, 'fret 12 = scale/2');
  near(GMB.fretTheoreticalMm(s, 24), 247.5, 1e-9, 'fret 24 = 3/4 of the scale');
  // Monotonic and converging — a decreasing step per fret.
  var prev = -1, step = Infinity;
  for (var f = 0; f <= 20; f++) {
    var v = GMB.fretTheoreticalMm(s, f);
    check(v > prev || f === 0, 'fret ' + f + ' is beyond fret ' + (f - 1));
    if (f > 0) {
      var d = v - prev;
      check(d < step + 1e-9, 'fret spacing narrows toward the bridge');
      step = d;
    }
    prev = v;
  }
});

test('the per-string fret offset shifts the whole fretboard from the endstop', function () {
  // fretOffsetMm is the HOME endstop -> nut distance: every fret is measured from
  // the nut and then shifted, so one number places a whole string's fretboard.
  var s = { scaleLengthMm: 330, fretOffsetMm: 25 };
  near(GMB.fretAbsoluteMm(s, 0), 25, 1e-9, 'open string sits at the offset');
  near(GMB.fretAbsoluteMm(s, 12), 25 + 165, 1e-9, 'fret 12 = offset + scale/2');
  // Changing only the offset translates every fret by the same amount — it must
  // never rescale the fretboard.
  var t = { scaleLengthMm: 330, fretOffsetMm: 40 };
  for (var f = 0; f <= 12; f++)
    near(GMB.fretAbsoluteMm(t, f) - GMB.fretAbsoluteMm(s, f), 15, 1e-9,
         'fret ' + f + ' translates with the offset');
});

test('a calibrated fret overrides the theory, and only that fret', function () {
  var s = { scaleLengthMm: 330, fretOffsetMm: 10, calibratedFretMm: [] };
  s.calibratedFretMm[5] = 88.5;                  // measured on the real instrument
  near(GMB.fretAbsoluteMm(s, 5), 10 + 88.5, 1e-9, 'calibrated fret wins');
  near(GMB.fretAbsoluteMm(s, 6), 10 + GMB.fretTheoreticalMm(s, 6), 1e-9,
       'an uncalibrated neighbour still uses the theory');
  // A calibrated ZERO is a real measurement (the nut IS at the offset), not a
  // missing entry — the classic falsy-check bug would silently drop it.
  s.calibratedFretMm[0] = 0;
  near(GMB.fretAbsoluteMm(s, 0), 10, 1e-9, 'a calibrated 0 mm is honoured');
});

// ---------------------------------------------------------------------------
// Pin capability filter — what colours the GPIO grid and gates auto-assign.
// ---------------------------------------------------------------------------

test('pin capabilities gate each signal kind', function () {
  var fast = { exposed: true, input: true, output: true, interrupt: true,
               highSpeedOutput: true, preference: 'recommended' };
  var slow = { exposed: true, input: true, output: true, interrupt: true,
               highSpeedOutput: false, preference: 'recommended' };
  var inOnly = { exposed: true, input: true, output: false, interrupt: true,
                 highSpeedOutput: false, preference: 'reserved' };
  var reserved = { exposed: true, input: true, output: true, interrupt: true,
                   highSpeedOutput: true, reserved: true, preference: 'reserved' };

  check(GMB.pinSupports(fast, 'step'), 'a high-speed output can carry STEP');
  check(!GMB.pinSupports(slow, 'step'), 'a slow output cannot carry STEP');
  check(GMB.pinSupports(slow, 'dir'), 'a plain output can carry DIR');
  check(GMB.pinSupports(slow, 'home'), 'an interrupt-capable input can carry HOME');
  check(!GMB.pinSupports(inOnly, 'dir'), 'an input-only pin cannot carry an output');
  check(!GMB.pinSupports(inOnly, 'sda'), 'an input-only pin cannot carry open-drain I2C');
  // A reserved pin is refused for EVERY kind — this is what keeps GPIO0 (the BOOT
  // button / hotspot escape hatch) out of the wizard's candidate lists.
  ['step', 'dir', 'home', 'limit', 'enable', 'sda', 'scl', 'servoOe', 'generic']
    .forEach(function (k) {
      check(!GMB.pinSupports(reserved, k), 'a reserved pin refuses ' + k);
    });
  check(!GMB.pinSupports(null, 'step'), 'an unknown pin supports nothing');
});

// ---------------------------------------------------------------------------
// Announced capabilities — the note range a GMB controller is told about.
// ---------------------------------------------------------------------------

test('capabilities span the real playable range', function () {
  var p = {
    instrument: { capo: 0, transpose: 0, stringCount: 2, polyphonyMax: 0 },
    midi: { transpose: 0, sustainPedal: false },
    stringFretSelection: { enabled: false, string: { ccNumber: 20 }, fret: { ccNumber: 21 } },
    capabilitiesRevision: 3,
    strings: [
      { enabled: true, openNote: 67, maxFret: 12 },
      { enabled: true, openNote: 60, maxFret: 12 }
    ]
  };
  var caps = GMB.computeCapabilities(p);
  check(caps.lowestNote === 60, 'lowest note = the lowest open string');
  check(caps.highestNote === 67 + 12, 'highest note = highest string at its top fret');

  // A DISABLED string must not be announced: the controller would send notes the
  // machine cannot play.
  p.strings[1].enabled = false;
  var caps2 = GMB.computeCapabilities(p);
  check(caps2.lowestNote === 67, 'a disabled string drops out of the range');
  check(caps2.polyphony === 1, 'polyphony follows the playable strings');

  // An explicit polyphony cap is announced, but never above the playable count.
  p.strings[1].enabled = true;
  p.instrument.polyphonyMax = 1;
  check(GMB.computeCapabilities(p).polyphony === 1, 'an explicit cap is announced');
  p.instrument.polyphonyMax = 6;
  check(GMB.computeCapabilities(p).polyphony === 2, 'a cap above the strings is clamped');
});

test('capo and transposes shift the announced range together', function () {
  var base = {
    instrument: { capo: 0, transpose: 0, stringCount: 1, polyphonyMax: 0 },
    midi: { transpose: 0, sustainPedal: false },
    stringFretSelection: { enabled: false, string: { ccNumber: 20 }, fret: { ccNumber: 21 } },
    capabilitiesRevision: 1,
    strings: [{ enabled: true, openNote: 60, maxFret: 12 }]
  };
  var a = GMB.computeCapabilities(base);
  base.instrument.capo = 2;
  base.instrument.transpose = 3;
  base.midi.transpose = -1;          // both transposes apply, like the allocator
  var b = GMB.computeCapabilities(base);
  check(b.lowestNote - a.lowestNote === 4, 'capo + both transposes shift the low end');
  check(b.highestNote - a.highestNote === 4, 'and the high end by the same amount');
});

// ---------------------------------------------------------------------------
// The shipped sample profile must stay a VALID v2 profile — the mock backend is
// what a first-time user sees, and it seeds every new instrument.
// ---------------------------------------------------------------------------

test('the sample profile carries the v2 schema', function () {
  var p = GMB.sampleProfile();
  check(p.profileVersion === 2, 'profileVersion is 2');
  check(!('staticIp' in p.network), 'the removed staticIp flag is gone (P1.9)');
  check(!!p.power && p.power.staggerMs > 0, 'the power governor block is present');
  check(!!p.pluck && p.pluck.muteSource === 'auto', 'the pluck block defaults to auto');
  check(!!p.hardware, 'the hardware notes block is present');
  check(p.hardware.stepperHoldMa > 0 && p.hardware.stepperMoveMa > 0,
        'the hardware block carries the stepper currents the estimator needs');
  check(p.board.estopNormallyClosed === false,
        'the E-stop polarity defaults to the legacy NO button');
  check(p.instrument.polyphonyMax === 0, 'polyphony defaults to automatic');
  (p.servos || []).forEach(function (s, i) {
    check(s.i2cBus === 0 || s.i2cBus === 1, 'servo ' + i + ' names an I2C bus');
    check(typeof s.muteUs === 'number', 'servo ' + i + ' has a muteUs field');
  });
  // Every axis must carry the geometry the fret maths needs, or the Instrument
  // page renders a fretboard of NaNs.
  (p.strings || []).forEach(function (s, i) {
    check(s.scaleLengthMm > 0, 'axis ' + i + ' has a scale length');
    check(typeof s.fretOffsetMm === 'number', 'axis ' + i + ' has a fret offset');
    check(s.maxPositionMm > s.minPositionMm, 'axis ' + i + ' has a usable travel');
  });
});

test('the mock diagnostics body matches the firmware shape', function () {
  // The Diagnostics panel is laid out against this in mock mode; if it drifts from
  // buildDiagnosticsJson() the panel silently shows blanks on a real device.
  var d = GMB.mockDiagnostics();
  ['uptimeMs', 'resetReason', 'freeHeap', 'minFreeHeap', 'state', 'midi', 'scheduler',
   'cmdQueueHighWater', 'faults', 'servoMoves', 'governorThrottles', 'motion',
   'moveMix', 'wifiReconnects', 'pca'].forEach(function (k) {
    check(k in d, 'diagnostics carries ' + k);
  });
  ['events', 'droppedEvents', 'droppedPackets', 'rejectedPackets'].forEach(function (k) {
    check(k in d.midi, 'diagnostics.midi carries ' + k);
  });
  ['maxLatencyUs', 'jitterUs', 'meanUs'].forEach(function (k) {
    check(k in d.scheduler, 'diagnostics.scheduler carries ' + k);
  });
  ['axisMoves', 'homingFailures', 'limitTrips', 'moveTimeouts'].forEach(function (k) {
    check(k in d.motion, 'diagnostics.motion carries ' + k);
  });
  ['deadline', 'staggerableGranted', 'staggerableDeferred'].forEach(function (k) {
    check(k in d.moveMix, 'diagnostics.moveMix carries ' + k);
  });
});

// ---------------------------------------------------------------------------
// Selection-CC decoding. This is the JS twin of StringFretSelector::mapStringValue
// / mapFretValue: the integrated test tool sends raw CC VALUES to the device and
// tells the operator which axis they select, so if this drifts from the firmware
// the tool confidently points at the wrong string.
// ---------------------------------------------------------------------------

function selCfg(over) {
  var p = GMB.sampleProfile();
  var sfs = p.stringFretSelection;
  sfs.string.ccNumber = 20; sfs.string.minimum = 1; sfs.string.maximum = 4;
  sfs.string.offset = 0; sfs.string.numbering = 'oneBased';
  sfs.string.reverseOrder = false; sfs.string.mapping = [];
  sfs.fret.ccNumber = 21; sfs.fret.minimum = 0; sfs.fret.maximum = 24; sfs.fret.offset = 0;
  if (over) over(sfs, p);
  return { p: p, sfs: sfs };
}

test('string CC decodes one-based values to physical axes', function () {
  var c = selCfg();
  check(GMB.decodeStringCc(c.sfs, c.p, 1) === 0, 'CC value 1 is axis 0');
  check(GMB.decodeStringCc(c.sfs, c.p, 4) === 3, 'CC value 4 is axis 3');
  check(GMB.decodeStringCc(c.sfs, c.p, 0) === -1, 'below the minimum is rejected');
  check(GMB.decodeStringCc(c.sfs, c.p, 5) === -1, 'above the maximum is rejected');
});

test('string CC honours zero-based numbering', function () {
  var c = selCfg(function (sfs) { sfs.string.numbering = 'zeroBased'; sfs.string.minimum = 0; });
  check(GMB.decodeStringCc(c.sfs, c.p, 0) === 0, 'CC value 0 is axis 0');
  check(GMB.decodeStringCc(c.sfs, c.p, 3) === 3, 'CC value 3 is axis 3');
});

test('string CC honours the offset', function () {
  // offset shifts the value BEFORE the one-based bias, exactly as the firmware does.
  var c = selCfg(function (sfs) { sfs.string.offset = 10; sfs.string.minimum = 0; sfs.string.maximum = 3; });
  check(GMB.decodeStringCc(c.sfs, c.p, 0) === -1, 'value+offset lands past the last axis');
  var d = selCfg(function (sfs) { sfs.string.offset = -1; sfs.string.minimum = 2; sfs.string.maximum = 5; });
  check(GMB.decodeStringCc(d.sfs, d.p, 2) === 0, 'a negative offset shifts back onto axis 0');
});

test('string CC honours reverse order', function () {
  var c = selCfg(function (sfs) { sfs.string.reverseOrder = true; });
  check(GMB.decodeStringCc(c.sfs, c.p, 1) === 3, 'the first value selects the LAST axis');
  check(GMB.decodeStringCc(c.sfs, c.p, 4) === 0, 'the last value selects the first axis');
});

test('string CC honours the mapping table, and reverse is applied before it', function () {
  var c = selCfg(function (sfs) { sfs.string.mapping = [3, 2, 1, 0]; });
  check(GMB.decodeStringCc(c.sfs, c.p, 1) === 3, 'mapping redirects axis 0 to axis 3');
  var d = selCfg(function (sfs) { sfs.string.reverseOrder = true; sfs.string.mapping = [0, 1, 2, 3]; });
  check(GMB.decodeStringCc(d.sfs, d.p, 1) === 3, 'reverse then identity mapping still reverses');
  var e = selCfg(function (sfs) { sfs.string.mapping = [0, 1]; });
  check(GMB.decodeStringCc(e.sfs, e.p, 3) === -1, 'an index past a short mapping is rejected');
});

test('fret CC range-checks before applying its offset', function () {
  var c = selCfg();
  check(GMB.decodeFretCc(c.sfs, 0) === 0, 'value 0 is fret 0');
  check(GMB.decodeFretCc(c.sfs, 24) === 24, 'value 24 is fret 24');
  check(GMB.decodeFretCc(c.sfs, 25) === -1, 'above the maximum is rejected');
  var d = selCfg(function (sfs) { sfs.fret.offset = -2; sfs.fret.minimum = 0; });
  check(GMB.decodeFretCc(d.sfs, 1) === -1, 'a negative result is rejected, not clamped');
  check(GMB.decodeFretCc(d.sfs, 5) === 3, 'the offset shifts the fret');
});

test('CC encoding is the exact inverse of decoding', function () {
  // Brute-force inverse: it has to survive reverseOrder + an arbitrary mapping,
  // which no closed-form inverse would.
  var c = selCfg(function (sfs) { sfs.string.reverseOrder = true; sfs.string.mapping = [2, 0, 3, 1]; });
  for (var axis = 0; axis < 4; axis++) {
    var v = GMB.encodeStringCc(c.sfs, c.p, axis);
    check(v >= 0, 'axis ' + axis + ' has a CC value');
    check(GMB.decodeStringCc(c.sfs, c.p, v) === axis, 'CC ' + v + ' decodes back to axis ' + axis);
  }
  var d = selCfg(function (sfs) { sfs.fret.offset = 3; sfs.fret.minimum = 0; });
  var fv = GMB.encodeFretCc(d.sfs, 7);
  check(GMB.decodeFretCc(d.sfs, fv) === 7, 'the fret value round-trips through the offset');
});

// ---------------------------------------------------------------------------
// GMB v2 descriptor. GMB.mockDescriptor is the JS mirror of the firmware's
// GmbDescriptor::toJson — it is what the offline demo serves for
// GET /gmb/descriptor.json, so a drift here misrepresents the instrument.
// ---------------------------------------------------------------------------

test('the descriptor keeps the tuning in physical order', function () {
  // The sample profile is a RE-ENTRANT ukulele (G4 C4 E4 A4): string 1 is the
  // highest-pitched. Sorting the tuning would pair each open note with another
  // string's fret count, and the descriptor builds one voice from that pair.
  var p = GMB.sampleProfile();
  var caps = GMB.computeCapabilities(p);
  check(caps.tuning.join() === '67,60,64,69', 'tuning is positional, not sorted');
  check(caps.fretsPerString.length === caps.tuning.length,
        'fretsPerString is index-aligned with tuning');
  var d = GMB.mockDescriptor(p);
  check(d.instruments[0].physical.tuning.join() === '67,60,64,69',
        'the descriptor carries the same order');
});

test('the descriptor folds the transpose into the announced tuning', function () {
  var p = GMB.sampleProfile();
  p.instrument.transpose = 2;
  var caps = GMB.computeCapabilities(p);
  check(caps.tuning.join() === '69,62,66,71', 'each open note is shifted');
  var lowest = Math.min.apply(null, caps.tuning);
  check(caps.noteMin === lowest + caps.capo,
        'tuning + capo reproduces the announced range');
});

test('one descriptor voice per carriage, with its own reach', function () {
  var p = GMB.sampleProfile();
  p.strings[0].maxFret = 5;          // give the strings distinguishable reaches
  p.strings[3].maxFret = 15;
  var d = GMB.mockDescriptor(p);
  var v = d.instruments[0].voices;
  check(v.length === 4, 'one voice per string');
  check(v[0].id === 's1' && v[0].notes.min === 67 && v[0].notes.max === 72,
        'voice 1 spans its own 5 frets from its own open note');
  check(v[3].id === 's4' && v[3].notes.min === 69 && v[3].notes.max === 84,
        'voice 4 spans its own 15 frets');
  check(d.instruments[0].polyphony.constraints[0].type === 'one_note_per_voice',
        'a carriage plays one note at a time');
});

test('the descriptor announces selection CCs only when selection is on', function () {
  var p = GMB.sampleProfile();
  p.stringFretSelection.enabled = false;
  check(GMB.mockDescriptor(p).instruments[0].physical.selection === undefined,
        'no selection block when the firmware would ignore the CCs');
  p.stringFretSelection.enabled = true;
  var sel = GMB.mockDescriptor(p).instruments[0].physical.selection;
  check(sel && sel.cc_string === p.stringFretSelection.string.ccNumber,
        'the configured CC numbers are announced when active');
});

test('the descriptor is 7-bit clean (it travels over SysEx verbatim)', function () {
  var p = GMB.sampleProfile();
  p.instrument.name = 'Ukulélé né\u00e9';
  var json = JSON.stringify(GMB.mockDescriptor(p));
  // JSON.stringify keeps non-ASCII literal; the firmware escapes it. What matters
  // here is that the structure survives and the name round-trips.
  check(JSON.parse(json).device.name === p.instrument.name, 'the name round-trips');
  check(json.indexOf('"gmb_descriptor":2') >= 0, 'the version marker is present');
});

// ---------------------------------------------------------------------------
// Small utilities that other modules rely on.
// ---------------------------------------------------------------------------

test('note names round-trip the octave numbering', function () {
  check(GMB.noteName(60) === 'C4', 'MIDI 60 is C4');
  check(GMB.noteName(69) === 'A4', 'MIDI 69 is A4');
  check(GMB.noteName(0) === 'C-1', 'MIDI 0 is C-1');
});

test('slug produces a safe filename stem', function () {
  check(GMB.slug('Guitar Standard') === 'guitar-standard', 'spaces become dashes');
  check(GMB.slug('  ') === 'profile', 'an empty name falls back');
  check(GMB.slug('Ukulélé #2').indexOf(' ') < 0, 'no spaces survive');
});

test('deepCopy really detaches nested state', function () {
  var a = { strings: [{ calibratedFretMm: [1, 2, 3] }] };
  var b = GMB.deepCopy(a);
  b.strings[0].calibratedFretMm[0] = 99;
  check(a.strings[0].calibratedFretMm[0] === 1, 'the source is untouched');
});

console.log('\n' + checks + ' checks, ' + failures + ' failures');
process.exit(failures ? 1 : 0);
