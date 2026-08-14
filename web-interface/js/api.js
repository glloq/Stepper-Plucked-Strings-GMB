/*
 * api.js — REST + WebSocket client for the Stepper-Plucked-Strings-GMB
 * web interface, with a self-contained MOCK mode.
 *
 * The page is served from the ESP32 over LittleFS, so this file talks to the
 * firmware REST API (see endpoint list in README.md). When no backend is
 * reachable — typically when index.html is opened directly from disk — every
 * call transparently falls back to an in-memory mock so the whole UI stays
 * usable standalone with realistic sample data (a 4-string GCEA ukulele).
 *
 * Everything is exposed on the global GMB namespace; no ES modules / no build
 * step, so it works from file:// where module imports would be blocked.
 */
(function (global) {
  'use strict';

  var GMB = global.GMB || (global.GMB = {});

  // ---------------------------------------------------------------------------
  // ESP32-S3-DevKitC-1 board profile (spec 11.4 / 11.5).
  //
  // Mirrors firmware/src/core/board/BoardProfile.h PinCapability. `preference`:
  //   recommended | caution | reserved   (grey "used" is a runtime state).
  // ESP32-S3 exposes GPIO0..21 and GPIO26..48 (22..25 do not exist).
  // ---------------------------------------------------------------------------
  function pin(gpio, opts) {
    opts = opts || {};
    return {
      gpio: gpio,
      exposed: opts.exposed !== false,
      input: opts.input !== false,
      output: opts.output !== false,
      interrupt: opts.interrupt !== false,
      highSpeedOutput: !!opts.highSpeedOutput,
      internalPullUp: opts.internalPullUp !== false,
      internalPullDown: opts.internalPullDown !== false,
      adc: !!opts.adc,
      reserved: !!opts.reserved,
      strapping: !!opts.strapping,
      usb: !!opts.usb,
      onboardPeripheral: !!opts.onboardPeripheral,
      preference: opts.preference || 'recommended',
      note: opts.note || ''
    };
  }

  // Board capability tables come from js/boarddata.js, GENERATED from
  // firmware/src/core/board/BoardProfile.cpp. api.js used to hand-build the S3
  // table here — a third copy of the same data, and the reason the board picker
  // offered one model while the firmware supported four.
  function boardList() {
    return (GMB.BOARD_PROFILES || []).map(function (b) {
      return { identifier: b.identifier, displayName: b.displayName };
    });
  }
  function boardById(id) {
    var all = GMB.BOARD_PROFILES || [];
    for (var i = 0; i < all.length; i++) if (all[i].identifier === id) return all[i];
    return all[0] || null;
  }

  // ---------------------------------------------------------------------------
  // Recommended pin assignment (spec 11.5), PER BOARD, from the generated tables.
  // Signal names match firmware PinAssignment.signal ("STEP1", "HOME3", "SDA"...).
  // This was one hard-coded S3 map, so auto-assign handed out S3 pins whichever
  // board was selected — on a classic ESP32 half of them do not exist.
  // ---------------------------------------------------------------------------
  GMB.recommendedFor = function (identifier) {
    var b = boardById(identifier);
    return (b && b.recommendedAssignment) ||
           { STEP: [], DIR: [], HOME: [], SDA: -1, SCL: -1, ENABLE: -1, SERVO_OE: -1 };
  };

  // Which capability a signal kind needs (mirrors BoardProfile::candidatesFor).
  var SIGNAL_KIND = {
    step: 'step', dir: 'dir', enable: 'enable', home: 'home', limit: 'limit',
    diag: 'diag', sda: 'i2cSda', scl: 'i2cScl', servoOe: 'servoOe', servo: 'servo'
  };
  GMB.SIGNAL_KIND = SIGNAL_KIND;

  // Fill the physical power/safety declaration block (HardwareNotes) with its
  // defaults IN PLACE, so a profile saved before the block existed (or loaded
  // from an older firmware) binds cleanly in the Power & safety / I²C & PCA
  // views — both bind their inputs straight to this object, so a missing field
  // would bind to undefined and the view would render blanks. Returns the block.
  //
  // The defaults MUST match ProfileStorage::fromJson()'s, or the UI would show a
  // value the firmware does not actually hold.
  // Fill in every section an imported profile may be missing, from the sample
  // profile's defaults. An import used to be adopted after a four-key shape check,
  // so a file with no `board`, `midi`, `stringFretSelection`, `power` or `pluck`
  // reached the views and surfaced later as blank fields or a thrown render. This
  // is the normalisation step between "parses as JSON" and "is a usable draft";
  // the device's validator still has the final say on the content.
  GMB.ensureProfileDefaults = function (p) {
    if (!p || typeof p !== 'object') return p;
    var d = sampleProfile();
    // Whole sections: adopt the default only when absent, never merged field by
    // field — a half-merged section is harder to reason about than a default one.
    ['instrument', 'board', 'hardware', 'network', 'midi', 'stringFretSelection',
     'power', 'pluck'].forEach(function (k) {
      if (!p[k] || typeof p[k] !== 'object') p[k] = d[k];
    });
    if (!Array.isArray(p.pins)) p.pins = [];
    if (!Array.isArray(p.servos)) p.servos = [];
    if (!Array.isArray(p.strings)) p.strings = [];
    if (!p.project) p.project = d.project;
    if (!(p.profileVersion >= 1)) p.profileVersion = d.profileVersion;
    if (!(p.capabilitiesRevision >= 0)) p.capabilitiesRevision = 0;
    // Per-string blocks the views bind to directly.
    p.strings.forEach(function (st) {
      if (!st.homing || typeof st.homing !== 'object') st.homing = d.strings[0].homing;
      if (!Array.isArray(st.calibratedFretMm)) st.calibratedFretMm = [];
      if (st.enabled === undefined) st.enabled = true;
    });
    GMB.ensureHardware(p);
    return p;
  };

  GMB.ensureHardware = function (p) {
    var hw = p.hardware || (p.hardware = {});
    if (hw.oePullup === undefined) hw.oePullup = false;
    if (hw.oeGate === undefined) hw.oeGate = false;
    if (hw.estopCutsPower === undefined) hw.estopCutsPower = false;
    // Stepper-only: the E-stop must also force the driver ENABLE inactive, since
    // a driver left energised still holds the carriage and still heats.
    if (hw.estopCutsDriverEnable === undefined) hw.estopCutsDriverEnable = false;
    if (hw.mainSwitch === undefined) hw.mainSwitch = false;
    if (hw.mainFuse === undefined) hw.mainFuse = false;
    if (hw.branchFuses === undefined) hw.branchFuses = false;
    if (!(hw.servoIdleMa >= 0)) hw.servoIdleMa = 10;
    if (!(hw.servoMoveMa >= 0)) hw.servoMoveMa = 250;
    if (!(hw.servoStallMa >= 0)) hw.servoStallMa = 800;
    // Per-axis stepper currents the motor-rail estimator sizes from.
    if (!(hw.stepperHoldMa >= 0)) hw.stepperHoldMa = 400;
    if (!(hw.stepperMoveMa >= 0)) hw.stepperMoveMa = 800;
    if (!(hw.extPullupOhm0 >= 0)) hw.extPullupOhm0 = 0;
    if (!(hw.extPullupOhm1 >= 0)) hw.extPullupOhm1 = 0;
    if (!Array.isArray(hw.pcaPullups)) hw.pcaPullups = [];
    return hw;
  };

  // Can a pin (statically) carry a given signal kind? (spec 11.3)
  GMB.pinSupports = function (p, kind) {
    if (!p || !p.exposed || p.reserved || p.preference === 'reserved') return false;
    switch (kind) {
      case 'step': return p.output && p.highSpeedOutput;
      case 'dir':
      case 'enable':
      case 'servo':    // direct-GPIO servo: LEDC 50 Hz PWM — any output pin
      case 'servoOe': return p.output;
      case 'home':
      case 'limit': return p.input && p.interrupt;
      case 'diag': return p.input;
      case 'i2cSda':
      case 'i2cScl': return p.input && p.output;
      default: return p.output;
    }
  };

  // ---------------------------------------------------------------------------
  // Mock axis positions, in mm from the homing zero. /api/test/jog and
  // /api/test/moveto write here and the status snapshot reads it, so offline the
  // carriages move for real instead of every control reporting a success that
  // changes nothing.
  // ---------------------------------------------------------------------------
  var mockAxes = [];
  function mockAxisPos(axis) { return mockAxes[axis] || 0; }
  function mockAxisMove(axis, positionMm) {
    var p = (global.GMB.state && global.GMB.state.profile) || MOCK.profile;
    var s = p && p.strings && p.strings[axis];
    if (!s) return { ok: false, error: 'no such axis' };
    // The firmware clamps to the axis travel rather than refusing, so an
    // out-of-range target parks at the limit here too.
    var lo = Number(s.minPositionMm) || 0;
    var hi = Number(s.maxPositionMm);
    if (!(hi > lo)) hi = lo;
    var v = Math.min(hi, Math.max(lo, Number(positionMm) || 0));
    mockAxes[axis] = v;
    return { ok: true, accepted: true, commandId: 0,
             note: 'axis ' + axis + ' -> ' + v.toFixed(2) + ' mm (mock)' };
  }

  // ---------------------------------------------------------------------------
  // Sample profile — a 4-string GCEA ukulele (reentrant tuning G4 C4 E4 A4).
  // Matches the JSON schema in the project brief exactly.
  // ---------------------------------------------------------------------------
  function ukuleleString(openNote) {
    return {
      enabled: true,
      openNote: openNote, maxFret: 12, scaleLengthMm: 330,
      transmission: 'beltGt2', stepsPerRevolution: 200, microsteps: 16,
      pulleyTeeth: 20, beltPitchMm: 2, leadPerRevolutionMm: 8, customStepsPerMm: 80,
      invertDirection: false, minPositionMm: 0, maxPositionMm: 300, fretOffsetMm: 0,
      maxSpeedMmS: 200, maxAccelMmS2: 2000, calibratedFretMm: [],
      homing: {
        direction: -1, fastSpeedMmS: 40, slowSpeedMmS: 5, backoffMm: 3, offsetMm: 0,
        timeoutMs: 8000, maxSearchMm: 500, sensorActiveHigh: true, limitActiveHigh: false
      }
    };
  }

  // A single servo entry (matches firmware ServoConfig). `source` is "pca" or
  // "gpio"; per-string servos carry stringIndex, shared/aux servos use -1.
  function servo(fn, stringIndex, opts) {
    opts = opts || {};
    return {
      enabled: opts.enabled !== false,
      function: fn,
      stringIndex: stringIndex === undefined ? -1 : stringIndex,
      source: opts.source || 'pca',   // "pca" | "gpio"
      pcaBoard: opts.pcaBoard || 0,   // 0..7 (0x40..0x47) within its bus
      // Which hardware I2C controller the board hangs off: 0 = Wire (SDA/SCL),
      // 1 = Wire1 (SDA2/SCL2). A board is identified by (i2cBus, pcaBoard).
      i2cBus: opts.i2cBus === 1 ? 1 : 0,
      channel: opts.channel === undefined ? 0 : opts.channel, // 0..15 (source == pca)
      gpio: opts.gpio === undefined ? -1 : opts.gpio,         // ESP32 GPIO (source == gpio)
      pulseMinUs: opts.pulseMinUs || 500,
      pulseMaxUs: opts.pulseMaxUs || 2500,
      restUs: opts.restUs || 1000,
      activeUs: opts.activeUs || 1800,
      // Plectrum-as-mute rest position (0 = none: the string rings, or a damper
      // servo mutes it). Only meaningful on a pluck/strum servo.
      muteUs: opts.muteUs || 0,
      inverted: !!opts.inverted,
      travelMs: opts.travelMs || 120,
      settleMs: opts.settleMs || 30,
      disableAtRest: opts.disableAtRest !== false,
      // Strum / pluck stroke shaping (matches firmware ServoConfig).
      engageDelayMs: opts.engageDelayMs || 0,
      alternateDirection: !!opts.alternateDirection,
      activeAltUs: opts.activeAltUs || 0,
      strokeMs: opts.strokeMs || 0,
      minStrikeUs: opts.minStrikeUs || 0
    };
  }
  GMB.servoDefaults = servo;

  // Theoretical fret position (spec 14.2), measured from the nut (fret 0 = 0):
  // scale·(1−2^(−fret/12)). The per-string fret offset (nut → FDC) is applied by
  // the firmware; the editor stores nut-relative values and shows the absolute.
  GMB.fretTheoreticalMm = function (s, fret) {
    return (s.scaleLengthMm || 0) * (1 - Math.pow(2, -fret / 12));
  };
  // Absolute fret position from the FDC = fret offset + nut-relative value.
  GMB.fretAbsoluteMm = function (s, fret) {
    var cal = s.calibratedFretMm && s.calibratedFretMm[fret];
    var nut = (cal === undefined || cal === null) ? GMB.fretTheoreticalMm(s, fret) : cal;
    return (s.fretOffsetMm || 0) + nut;
  };

  function sampleProfile() {
    return {
      project: 'Stepper-Plucked-Strings-GMB', profileVersion: 2, capabilitiesRevision: 7,
      instrument: {
        name: 'Ukulele GCEA', description: '4-string soprano ukulele',
        stringCount: 4, type: 'ukulele', gmProgram: 24, typeId: 4,
        capo: 0, transpose: 0, polyphonyMax: 0
      },
      board: { profile: 'esp32-s3-devkitc-1', reserveUsb: true, automaticPinAssignment: true,
        estopNormallyClosed: false },
      pins: [
        { signal: 'STEP1', kind: 'step', gpio: 4 }, { signal: 'STEP2', kind: 'step', gpio: 5 },
        { signal: 'STEP3', kind: 'step', gpio: 6 }, { signal: 'STEP4', kind: 'step', gpio: 7 },
        { signal: 'DIR1', kind: 'dir', gpio: 17 }, { signal: 'DIR2', kind: 'dir', gpio: 18 },
        { signal: 'DIR3', kind: 'dir', gpio: 8 }, { signal: 'DIR4', kind: 'dir', gpio: 9 },
        { signal: 'HOME1', kind: 'home', gpio: 12 }, { signal: 'HOME2', kind: 'home', gpio: 13 },
        { signal: 'HOME3', kind: 'home', gpio: 14 }, { signal: 'HOME4', kind: 'home', gpio: 21 },
        { signal: 'SDA', kind: 'sda', gpio: 40 }, { signal: 'SCL', kind: 'scl', gpio: 41 },
        { signal: 'ENABLE', kind: 'enable', gpio: 42 }, { signal: 'SERVO_OE', kind: 'servoOe', gpio: 47 }
      ],
      hardware: {
        oePullup: false, oeGate: false, estopCutsPower: false,
        estopCutsDriverEnable: false, mainSwitch: false, mainFuse: false,
        branchFuses: false, servoIdleMa: 10, servoMoveMa: 250, servoStallMa: 800,
        stepperHoldMa: 400, stepperMoveMa: 800,
        extPullupOhm0: 0, extPullupOhm1: 0, pcaPullups: []
      },
      network: {
        mode: 'accessPoint', ssid: '', hostname: 'gmb-instrument',
        apSsid: 'Stepper-Plucked-Strings-GMB'
      },
      midi: {
        globalChannel: 0, omni: false, transpose: 0, chordWindowMs: 3,
        velocityCurve: 'linear', sustainPedal: true, sustainCc: 64,
        saturationStrategy: 'priorityLow',
        noteExecutionDelayMs: 0, fingerLeadMs: 0, strumLeadMs: 0
      },
      stringFretSelection: {
        enabled: true, mode: 'hybrid', preset: 'general-midi-boop', perMidiChannel: true,
        selectionTimeoutMs: 100, prepareOnCompleteSelection: true, queueDepth: 32,
        string: { ccNumber: 20, minimum: 1, maximum: 4, offset: 0, numbering: 'oneBased',
          reverseOrder: false, mapping: [0, 1, 2, 3] },
        fret: { ccNumber: 21, minimum: 0, maximum: 12, offset: 0, invalidValuePolicy: 'automaticFallback' },
        validation: {
          notePositionPolicy: 'ccPriorityWithWarning',
          missingSelectionPolicy: 'automaticAllocation',
          expiredSelectionPolicy: 'automaticAllocation'
        }
      },
      power: { maxConcurrentMoves: 3, maxConcurrentPerBoard: 0, staggerMs: 8 },
      pluck: {
        strokeMs: 0, minStrikePct: 0, fretToPluckMs: 0, muteSource: 'auto',
        muteHoldMs: 60, liftMuteOnNoteOff: false, liftEngage: 'lowerToPlay'
      },
      // Ukulele GCEA: physical order low->high used by GMB = G4(67) C4(60) E4(64) A4(69)
      strings: [ukuleleString(67), ukuleleString(60), ukuleleString(64), ukuleleString(69)],
      // A representative mix: one finger + one pluck per string on PCA board 0
      // (channels 0–3 fingers, 6–9 plucks — the recommended layout).
      servos: [
        servo('finger', 0, { channel: 0 }),
        servo('finger', 1, { channel: 1 }),
        servo('finger', 2, { channel: 2 }),
        servo('finger', 3, { channel: 3 }),
        servo('pluck', 0, { channel: 6, activeUs: 1700, travelMs: 90, settleMs: 20 }),
        servo('pluck', 1, { channel: 7, activeUs: 1700, travelMs: 90, settleMs: 20 }),
        servo('pluck', 2, { channel: 8, activeUs: 1700, travelMs: 90, settleMs: 20 }),
        servo('pluck', 3, { channel: 9, activeUs: 1700, travelMs: 90, settleMs: 20 })
      ]
    };
  }
  GMB.sampleProfile = sampleProfile;

  var NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
  GMB.noteName = function (n) {
    if (n === null || n === undefined || n < 0) return '--';
    return NOTE_NAMES[n % 12] + (Math.floor(n / 12) - 1);
  };

  // ---- selection-CC decoding (mirrors StringFretSelector::mapStringValue /
  // mapFretValue) --------------------------------------------------------------
  //
  // These answer "which physical axis / fret does CC value V select, under THIS
  // configuration?" — the question the integrated test tool exists to settle. The
  // order of operations matters and is the firmware's: range-check the raw value,
  // apply the offset, drop the one-based bias, reverse, then the mapping table.
  // Both return -1 for a value the firmware would reject.
  GMB.decodeStringCc = function (sfs, p, rawValue) {
    var cfg = sfs.string, count = p.instrument.stringCount;
    // Offset FIRST, then the range check — "logical = CC + offset, then
    // validated". Checking the raw value would shift the accepted band by the
    // offset, so a configured offset accepts the wrong values and rejects the
    // right ones.
    var index = rawValue + (cfg.offset || 0);
    if (index < cfg.minimum || index > cfg.maximum) return -1;
    if (cfg.numbering !== 'zeroBased') index -= 1;   // oneBased is the default
    if (index < 0 || index >= count) return -1;
    if (cfg.reverseOrder) index = (count - 1) - index;
    var map = cfg.mapping;
    if (map && map.length) {
      if (index >= map.length) return -1;
      index = map[index];
    }
    if (index < 0 || index >= count) return -1;
    return index;
  };
  GMB.decodeFretCc = function (sfs, rawValue) {
    var cfg = sfs.fret;
    var fret = rawValue + (cfg.offset || 0);   // offset first, same rule as above
    if (fret < 0) return -1;
    if (fret < cfg.minimum || fret > cfg.maximum) return -1;
    return fret;
  };
  // The inverse, for pre-filling the tester from a string the user picked. Brute
  // force over the 128 CC values rather than an algebraic inverse: reverseOrder
  // plus an arbitrary mapping table is not invertible in closed form, and a wrong
  // inverse would silently test the wrong axis.
  GMB.encodeStringCc = function (sfs, p, axis) {
    for (var v = 0; v <= 127; v++) if (GMB.decodeStringCc(sfs, p, v) === axis) return v;
    return -1;
  };
  GMB.encodeFretCc = function (sfs, fret) {
    for (var v = 0; v <= 127; v++) if (GMB.decodeFretCc(sfs, v) === fret) return v;
    return -1;
  };

  // Derive read-only capabilities from a profile (SysEx spec 5 / 6 / 17).
  GMB.computeCapabilities = function (p) {
    // Only ENABLED strings are announced, and the pitch shift is capo + BOTH
    // transposes — exactly what the allocator and the firmware's Capabilities
    // apply (frettedNote = open + fret + capo + transpose). Announcing a disabled
    // string, or forgetting a transpose, tells a controller to send notes the
    // machine cannot actually play.
    var strings = (p.strings || []).filter(function (s) { return s.enabled !== false; });
    var shift = (p.instrument.capo || 0) + (p.instrument.transpose || 0) +
                ((p.midi && p.midi.transpose) || 0);
    var notesSet = {};
    var min = 127, max = 0;
    strings.forEach(function (s) {
      var lo = s.openNote + shift;
      var hi = lo + (s.maxFret || 0);
      for (var n = lo; n <= hi; n++) { notesSet[n] = true; if (n < min) min = n; if (n > max) max = n; }
    });
    if (!strings.length) { min = 0; max = 0; }
    var allContinuous = true;
    for (var n2 = min; n2 <= max; n2++) if (!notesSet[n2]) { allContinuous = false; break; }
    var sfs = p.stringFretSelection;
    // PHYSICAL string order, index-aligned with fretsPerString, and folding the
    // transposes into the announced open pitch so tuning + capo reproduces the
    // announced range. Mirrors buildSnapshot() in the firmware; the GMB v2
    // descriptor pairs tuning[i] with fretsPerString[i] to describe voice i, so a
    // sorted tuning would misdescribe every voice of a re-entrant instrument.
    var openShift = (p.instrument.transpose || 0) + ((p.midi && p.midi.transpose) || 0);
    var tuning = strings.map(function (s) {
      return Math.max(0, Math.min(127, s.openNote + openShift));
    });
    var fretsPerString = strings.map(function (s) { return s.maxFret || 0; });
    var discreteNotes = [];
    if (!allContinuous) {
      for (var d = min; d <= max; d++) if (notesSet[d]) discreteNotes.push(d);
    }
    return {
      strings: p.instrument.stringCount,
      frets: Math.max.apply(null, strings.map(function (s) { return s.maxFret; }).concat([0])),
      fretsPerString: fretsPerString,
      discreteNotes: discreteNotes,
      noteMin: min, noteMax: max, noteMode: allContinuous ? 0 : 1,
      // Aliases used by the Instrument page / tests; same values, clearer names.
      lowestNote: min, highestNote: max,
      // Announced polyphony: 0 = automatic (= the playable strings); a custom cap
      // is never announced above the physical string count.
      polyphony: p.instrument.polyphonyMax
        ? Math.min(p.instrument.polyphonyMax, strings.length) : strings.length,
      ccString: sfs.string.ccNumber, ccFret: sfs.fret.ccNumber,
      ccActive: sfs.enabled ? 1 : 0,
      tuning: tuning, tuningNames: tuning.map(GMB.noteName),
      capo: p.instrument.capo || 0,
      revision: p.capabilitiesRevision,
      supportedCc: buildSupportedCc(p)
    };
  };

  function buildSupportedCc(p) {
    // Announce only enabled CCs (SysEx spec 7).
    var cc = [7, 11];
    if (p.stringFretSelection.enabled) {
      cc.push(p.stringFretSelection.string.ccNumber);
      cc.push(p.stringFretSelection.fret.ccNumber);
    }
    if (p.midi.sustainPedal) cc.push(64);
    cc.push(120, 123);
    return cc.sort(function (a, b) { return a - b; });
  }

  // ---------------------------------------------------------------------------
  // Live status (dashboard, spec 19). Mock evolves over time.
  // ---------------------------------------------------------------------------
  function sampleStatus() {
    var p = MOCK.profile;
    var caps = GMB.computeCapabilities(p);
    return {
      state: 'READY',
      wifi: { mode: p.network.mode, ssid: p.network.mode === 'station' ? p.network.ssid : p.network.apSsid,
        ip: p.network.mode === 'station' ? '192.168.1.42' : '192.168.4.1', rssi: -54, connected: true },
      midiSource: 'wifiUdp',
      // UDP source posture (P1.11) so the Settings panel shows the live state.
      midiSourcePolicy: MOCK.midiSourcePolicy,
      midiSourceLocked: MOCK.midiSourceLocked,
      safety: 'armed',
      activeProfile: p.instrument.name,
      stringsReady: p.instrument.stringCount, stringsTotal: p.instrument.stringCount,
      notesPlaying: 0,
      faults: [],
      capabilitiesRevision: p.capabilitiesRevision,
      temperatures: [{ name: 'Driver board', c: 34.2 }],
      voltages: [{ name: '24V motor', v: 24.1 }, { name: '5V servo', v: 5.02 }],
      strings: p.strings.map(function (s, i) {
        var pos = mockAxisPos(i);
        return {
          index: i, state: 'IDLE',
          note: null, fret: null,
          // Report where the mock jog / go-to-position actually left the carriage
          // instead of a hard-coded 0: a demo whose axes never move cannot show
          // that the jog and calibration controls do anything.
          positionMm: pos, targetMm: pos, distanceMm: 0,
          home: pos <= 0.05, limit: false,
          finger: 'up', plectrum: 'rest', lastFault: 'none',
          openNote: s.openNote
        };
      })
    };
  }

  // ---------------------------------------------------------------------------
  // Mock store.
  //
  // `slots` mirrors the firmware's slotted ProfileStorage: an array where each
  // entry is null (empty slot) or a full profile object. GET /api/profiles
  // derives { profiles:[{slot,name,used}], startupSlot } from it.
  // ---------------------------------------------------------------------------
  function demoProfile(name, type) {
    var p = sampleProfile();
    p.instrument.name = name;
    p.instrument.type = type;
    return p;
  }

  var MOCK = {
    profile: sampleProfile(),
    slots: [
      sampleProfile(),
      demoProfile('Guitar Standard', 'guitar'),
      demoProfile('Bass EADG', 'bass'),
      null, null, null, null, null
    ],
    startupSlot: 0,
    scanStartedAt: 0,             // mock Wi-Fi survey start (see api.wifiScan)
    midiSourcePolicy: 'open',     // UDP source posture (P1.11)
    midiSourceLocked: false
  };
  // The board the DRAFT selects, not a fixed one: changing the board model has to
  // change which GPIOs the mock offers and validates, or the picker would be
  // decorative offline.
  function currentBoard(profile) {
    var p = profile || (global.GMB.state && global.GMB.state.profile) || MOCK.profile;
    return boardById(p && p.board && p.board.profile) || { pins: [] };
  }
  GMB.mockBoard = currentBoard;

  // A plausible GET /api/diagnostics body for mock mode, so the Diagnostics panel
  // can be laid out and read without a device attached. Shape matches
  // buildDiagnosticsJson() in firmware/src/main.cpp exactly.
  var mockBootMs = nowMs();
  function mockDiagnostics() {
    var up = nowMs() - mockBootMs;
    return {
      uptimeMs: up, resetReason: 'powerOn', freeHeap: 214000, minFreeHeap: 198000,
      state: 'ready',
      midi: { events: Math.round(up / 900), droppedEvents: 0, droppedPackets: 0,
              rejectedPackets: 0 },
      scheduler: { maxLatencyUs: 2400, jitterUs: 380, meanUs: 1050 },
      cmdQueueHighWater: 2,
      faults: 0,
      servoMoves: Math.round(up / 1500),
      governorThrottles: 4,
      motion: { axisMoves: Math.round(up / 2600), homingFailures: 0, limitTrips: 0,
                moveTimeouts: 0 },
      moveMix: { deadline: Math.round(up / 3000), staggerableGranted: Math.round(up / 2600),
                 staggerableDeferred: 4 },
      wifiReconnects: 0,
      pca: { used: true, healthy: true }
    };
  }
  GMB.mockDiagnostics = mockDiagnostics;

  // Build the { profiles:[...], startupSlot } list from the slot array.
  function mockProfilesList() {
    var profiles = MOCK.slots.map(function (p, i) {
      return { slot: i, name: p ? (p.instrument.name || '') : '', used: !!p };
    });
    return { profiles: profiles, startupSlot: MOCK.startupSlot };
  }

  // ---------------------------------------------------------------------------
  // Mock auto-assign — reproduces the recommended profile, respecting the
  // requested string count and reserved USB pins (mirrors PinManager::autoAssign).
  // ---------------------------------------------------------------------------
  function mockAutoAssign(req) {
    var profile = (global.GMB.state && global.GMB.state.profile) || MOCK.profile;
    var n = req.stringCount || profile.instrument.stringCount;
    var rec = GMB.recommendedFor(req.board || (profile.board && profile.board.profile));
    var pins = [];
    var errors = [];
    for (var i = 0; i < n; i++) {
      // A classic ESP32 runs out of comfortable high-speed outputs well before 6
      // axes. Say so instead of emitting an undefined GPIO that later surfaces as
      // a mystery validation error.
      if (rec.STEP[i] === undefined || rec.DIR[i] === undefined || rec.HOME[i] === undefined) {
        errors.push({ signal: 'STEP' + (i + 1),
          reason: 'This board has no recommended pin set left for axis ' + (i + 1) +
                  '. Assign it by hand, or pick a board with more usable GPIOs.' });
        continue;
      }
      pins.push({ signal: 'STEP' + (i + 1), kind: 'step', gpio: rec.STEP[i] });
      pins.push({ signal: 'DIR' + (i + 1), kind: 'dir', gpio: rec.DIR[i] });
      pins.push({ signal: 'HOME' + (i + 1), kind: 'home', gpio: rec.HOME[i] });
    }
    if (req.useI2cServos !== false) {
      pins.push({ signal: 'SDA', kind: 'sda', gpio: rec.SDA });
      pins.push({ signal: 'SCL', kind: 'scl', gpio: rec.SCL });
    }
    if (req.globalEnable !== false) pins.push({ signal: 'ENABLE', kind: 'enable', gpio: rec.ENABLE });
    if (req.servoSafetyOe !== false) pins.push({ signal: 'SERVO_OE', kind: 'servoOe', gpio: rec.SERVO_OE });
    return { pins: pins, errors: errors };
  }

  // Mock validation (spec 11.6). Mirrors the firmware contract:
  // decodes the full profile and returns { ok, issues:[{field,message,severity}] }.
  function mockValidatePins(profile) {
    var pins = (profile && profile.pins) || [];
    var board = currentBoard(profile);
    var reserveUsb = !!(profile && profile.board && profile.board.reserveUsb);
    var errors = [];
    var byGpio = {};
    // Pins already handed out as suggestions in THIS pass. Without it every one of
    // eight broken signals was told to "try GPIO 16" — advice that cannot be
    // followed more than once.
    var claimed = {};
    pins.forEach(function (a) {
      if (a.gpio < 0) return;
      var cap = board.pins.filter(function (p) { return p.gpio === a.gpio; })[0];
      // Duplicate use.
      if (byGpio[a.gpio]) {
        errors.push({ signal: a.signal, gpio: a.gpio,
          reason: 'GPIO ' + a.gpio + ' is already used by ' + byGpio[a.gpio] + '.',
          suggestion: suggest(a.kind, pins, board, claimed), conflictWith: byGpio[a.gpio] });
      } else {
        byGpio[a.gpio] = a.signal;
      }
      // Reserved / USB / capability.
      if (!cap) {
        errors.push({ signal: a.signal, gpio: a.gpio,
          reason: 'GPIO ' + a.gpio + ' does not exist on this board.',
          suggestion: suggest(a.kind, pins, board, claimed), conflictWith: '' });
      } else if (reserveUsb && cap.usb) {
        errors.push({ signal: a.signal, gpio: a.gpio,
          reason: 'GPIO ' + a.gpio + ' is reserved for future native USB.',
          suggestion: suggest(a.kind, pins, board, claimed), conflictWith: 'USB (reserved)' });
      } else if (cap.preference === 'reserved') {
        errors.push({ signal: a.signal, gpio: a.gpio,
          reason: cap.note || ('GPIO ' + a.gpio + ' is reserved.'),
          suggestion: suggest(a.kind, pins, board, claimed), conflictWith: '' });
      } else if (!GMB.pinSupports(cap, SIGNAL_KIND[a.kind] || 'generic')) {
        errors.push({ signal: a.signal, gpio: a.gpio,
          reason: 'GPIO ' + a.gpio + ' is not compatible with a ' + a.kind.toUpperCase() + ' signal.',
          suggestion: suggest(a.kind, pins, board, claimed), conflictWith: '' });
      }
    });
    // Map the rich mock errors onto the firmware's { field, message, severity }
    // issue shape.
    var issues = errors.map(function (e) {
      // Board notes come from the generated tables and do not all end in a full
      // stop, so join rather than concatenate ("...SPI flash Try GPIO 16.").
      var reason = /[.!?]$/.test(e.reason) ? e.reason : e.reason + '.';
      var msg = reason + (e.suggestion ? ' ' + e.suggestion : '');
      return { field: e.signal + ' (GPIO' + e.gpio + ')', message: msg, severity: 'error' };
    });
    return { ok: issues.length === 0, issues: issues };
  }

  function suggest(kind, pins, board, claimed) {
    var used = {};
    pins.forEach(function (a) { used[a.gpio] = true; });
    var wantKind = SIGNAL_KIND[kind] || 'generic';
    var free = (board || currentBoard()).pins.filter(function (p) {
      return !used[p.gpio] && !(claimed && claimed[p.gpio]) &&
             p.preference === 'recommended' && GMB.pinSupports(p, wantKind);
    });
    if (!free.length) return 'No free recommended pin — free one up first.';
    if (claimed) claimed[free[0].gpio] = true;   // do not offer it twice
    return 'Try GPIO ' + free[0].gpio + '.';
  }

  // ---------------------------------------------------------------------------
  // SysEx mock (Communication ... SysEx spec). Builds byte arrays + decoded view.
  // ---------------------------------------------------------------------------
  var HEADER = [0xF0, 0x7D, 0x00];
  function hex(bytes) { return bytes.map(function (b) { return ('0' + b.toString(16)).slice(-2).toUpperCase(); }).join(' '); }
  GMB.hex = hex;

  // Block ids. dir 0x00 = request (host->device). Identity (0x01) now answers with
  // the GMB v2 handshake; the change notification is the v2 block 0x11. Blocks
  // 5/6/7 remain for the deprecated fixed-block path the firmware still serves.
  var BLOCK = { identity: 0x01, descriptor: 0x05, capabilities: 0x06, stringConfig: 0x07,
                notify: 0x11, descriptorTransfer: 0x10 };

  // Build the raw request bytes for a block. Channel-bearing blocks
  // (capabilities 0x06, stringConfig 0x07) carry the MIDI channel byte.
  // Returns null for spontaneous, device-emitted blocks (notify).
  function buildSysexRequest(kind, channel) {
    var ch = (channel || 0) & 0x7F;
    switch (kind) {
      case 'identity':     return HEADER.concat([BLOCK.identity, 0x00, 0xF7]);
      case 'descriptor':   return HEADER.concat([BLOCK.descriptor, 0x00, 0xF7]);
      case 'capabilities': return HEADER.concat([BLOCK.capabilities, 0x00, ch, 0xF7]);
      case 'stringConfig': return HEADER.concat([BLOCK.stringConfig, 0x00, ch, 0xF7]);
      default:             return null;   // notify has no host->device request
    }
  }
  GMB.buildSysexRequest = buildSysexRequest;

  // Build the GMB v2 block-0x11 change-notification (device-emitted):
  // F0 7D 00 11 02 <revision[5]> <flags> F7. Flags bit 1 = INSTRUMENTS_CHANGED.
  function buildNotifyMessage(p) {
    var caps = GMB.computeCapabilities(p);
    return HEADER.concat([BLOCK.notify, 0x02])
      .concat(encodeRevision(caps.revision)).concat([0x02, 0xF7]);
  }

  // The GMB v2 descriptor object for a profile — the JS mirror of the firmware's
  // GmbDescriptor::toJson. Used by the offline mock of GET /gmb/descriptor.json,
  // and to size the handshake's descriptor_size field.
  GMB.mockDescriptor = function (p) {
    var caps = GMB.computeCapabilities(p);
    var enabled = (p.strings || []).filter(function (s) { return s.enabled !== false; });
    var sfs = p.stringFretSelection;
    var notes = caps.noteMode === 0
      ? { mode: 'range', min: caps.noteMin, max: caps.noteMax }
      : { mode: 'discrete', list: caps.discreteNotes };
    // One voice per physical carriage. On this machine that IS the polyphony
    // limit: a carriage plays one note at a time, wherever it happens to be.
    var voices = caps.tuning.map(function (t, i) {
      var lo = Math.max(0, Math.min(127, t + caps.capo));
      var reach = caps.fretsPerString[i] != null ? caps.fretsPerString[i] : caps.frets;
      return { id: 's' + (i + 1),
               notes: { mode: 'range', min: lo, max: Math.max(0, Math.min(127, lo + reach)) } };
    });
    var physical = {
      family: 'strings', string_count: enabled.length, fret_count: caps.frets,
      frets_per_string: caps.fretsPerString, fretless: false, capo: caps.capo,
      tuning: caps.tuning,
      string_order: (sfs.string && sfs.string.reverseOrder) ? 'reversed'
        : ((sfs.string && sfs.string.mapping && sfs.string.mapping.length) ? 'custom' : 'normal')
    };
    // Announce the selection CCs only when selection is actually active: an
    // absent block means "unknown" and the controller keeps its own, whereas
    // announcing them would falsely enable CCs the firmware ignores.
    if (sfs.enabled) {
      physical.selection = {
        mode: sfs.mode,
        cc_string: sfs.string.ccNumber, cc_string_min: sfs.string.minimum,
        cc_string_max: sfs.string.maximum, cc_string_offset: sfs.string.offset || 0,
        cc_fret: sfs.fret.ccNumber, cc_fret_min: sfs.fret.minimum,
        cc_fret_max: sfs.fret.maximum, cc_fret_offset: sfs.fret.offset || 0
      };
    }
    var inst = {
      channel: p.midi.omni ? 0 : p.midi.globalChannel, configured: true,
      name: p.instrument.name || '', gm_program: p.instrument.gmProgram,
      notes: notes,
      polyphony: { max: caps.polyphony, constraints: [{ type: 'one_note_per_voice' }] },
      expression: { cc: caps.supportedCc }, voices: voices, physical: physical
    };
    if (p.instrument.type) inst.type = p.instrument.type;
    return {
      gmb_descriptor: 2, revision: caps.revision,
      device: { name: p.instrument.name || '',
                model: p.project || 'Stepper-Plucked-Strings-GMB' },
      instruments: [inst]
    };
  };

  // MOCK backend for POST /api/sysex/request: accepts { bytes:[...] } and
  // returns { ok:true, response:[...] } — the raw response bytes for the block
  // named in the request, built from the active mock profile.
  function mockSysExBackend(body) {
    var bytes = (body && body.bytes) || [];
    return { ok: true, response: mockSysexResponse(bytes) };
  }
  GMB.mockSysEx = mockSysExBackend;

  function mockSysexResponse(bytes) {
    if (!bytes || bytes.length < 5) return [];
    var block = bytes[3];
    var p = MOCK.profile, caps = GMB.computeCapabilities(p);
    var name = p.instrument.name || '';
    switch (block) {
      case BLOCK.identity: {
        // GMB v2 24-byte handshake: F0 7D 00 01 01 <proto=02> <instance_id[5]>
        // <fw[3]> <descriptor_size[3] LE> <revision[5] LE> <flags> F7.
        var dsz = JSON.stringify(GMB.mockDescriptor(p)).length;
        return HEADER.concat([BLOCK.identity, 0x01, 0x02])   // dir=response, proto_ver=2
          .concat([0x01, 0x02, 0x03, 0x04, 0x05])            // instance_id[5]
          .concat([0x01, 0x00, 0x00])                        // firmware 1.0.0
          .concat([dsz & 0x7F, (dsz >> 7) & 0x7F, (dsz >> 14) & 0x7F])  // descriptor_size[3]
          .concat(encodeRevision(caps.revision))             // revision[5]
          .concat([0x03, 0xF7]);                             // flags (HTTP+push), end
      }
      case BLOCK.descriptor:
        return HEADER.concat([BLOCK.descriptor, 0x01, 0x01, 0x01,
          p.midi.globalChannel, p.instrument.gmProgram, p.instrument.typeId, 0xF7]);
      case BLOCK.capabilities: {
        var ch = bytes[5] & 0x7F;
        return HEADER.concat([BLOCK.capabilities, 0x01, 0x01, ch, p.instrument.gmProgram, p.instrument.typeId,
          0x00, caps.noteMode, caps.noteMin, caps.noteMax, caps.polyphony, 0x00,
          caps.supportedCc.length]).concat(caps.supportedCc)
          .concat([name.length]).concat(strBytes(name, name.length))
          .concat([0xF7]);
      }
      case BLOCK.stringConfig: {
        var ch2 = bytes[5] & 0x7F;
        return HEADER.concat([BLOCK.stringConfig, 0x01, 0x01, ch2, caps.strings, caps.frets, 0x00, caps.capo,
          caps.ccActive, caps.ccString, caps.ccFret]).concat(caps.tuning).concat([0xF7]);
      }
      default:
        return [];
    }
  }

  // Decode raw response bytes for a block into a { field: value } display map,
  // by parsing the bytes (works for both the mock and the real firmware, which
  // share the SysEx wire layout).
  function decodeSysexResponse(kind, resp) {
    if (!resp || !resp.length) return {};
    try {
      switch (kind) {
        case 'identity': {
          // GMB v2 handshake (24 bytes). `Level` is what a controller acts on:
          // a non-zero descriptor size means the rich JSON descriptor is there.
          var dsz = (resp[14] || 0) | ((resp[15] || 0) << 7) | ((resp[16] || 0) << 14);
          var flg = resp[22] || 0;
          return { 'Proto ver': resp[5], 'Instance id': hex(resp.slice(6, 11)),
            Firmware: resp[11] + '.' + resp[12] + '.' + resp[13],
            'Descriptor size': dsz + ' B', Level: dsz > 0 ? '1 (descriptor)' : '0 (basic)',
            Revision: decodeRevision(resp.slice(17, 22)),
            HTTP: (flg & 1) ? 'yes' : 'no', Push: (flg & 2) ? 'yes' : 'no' };
        }
        case 'descriptor':
          return { Channel: resp[7] + 1, 'GM program': resp[8],
            'Type id': '0x0' + (resp[9] || 0).toString(16) };
        case 'capabilities': {
          var ch = resp[6], noteMode = resp[10], noteMin = resp[11], noteMax = resp[12],
            poly = resp[13], ccLen = resp[15];
          var cc = resp.slice(16, 16 + ccLen);
          return { Channel: ch + 1, Range: GMB.noteName(noteMin) + ' .. ' + GMB.noteName(noteMax),
            'Note mode': noteMode === 0 ? 'continuous' : 'discrete', Polyphony: poly,
            CC: cc.join(', ') };
        }
        case 'stringConfig': {
          var strings = resp[7], frets = resp[8], capo = resp[10],
            ccString = resp[12], ccFret = resp[13];
          var tuning = resp.slice(14, 14 + strings);
          return { Channel: resp[6] + 1, Strings: strings, Frets: frets, Fretless: 'no',
            Capo: capo, 'CC string': ccString, 'CC fret': ccFret,
            Tuning: tuning.map(GMB.noteName).join(' ') };
        }
        case 'notify':
          // v2 block 0x11: F0 7D 00 11 02 <revision[5]> <flags> F7 — no channel
          // byte any more, so reading one would report the revision's first byte.
          return { Revision: decodeRevision(resp.slice(5, 10)),
            Flags: '0x' + (resp[10] || 0).toString(16),
            Meaning: ((resp[10] || 0) & 0x02) ? 'instruments changed' : 'other' };
        default:
          return {};
      }
    } catch (e) { return { error: String(e) }; }
  }

  // Assemble the display view the SysEx tester renders (request/response hex,
  // decoded fields, 7-bit validity, byte count).
  function makeSysexView(kind, req, resp, t0) {
    var valid = resp.every(function (b) { return b === 0xF0 || b === 0xF7 || (b >= 0 && b <= 0x7F); });
    return {
      kind: kind, request: req ? hex(req) : '(spontaneous notification)',
      response: hex(resp), decoded: decodeSysexResponse(kind, resp), valid: valid,
      length: resp.length, durationMs: +(nowMs() - t0 + 1.2).toFixed(2), error: ''
    };
  }

  function nowMs() { return (typeof performance !== 'undefined' && performance.now) ? performance.now() : Date.now(); }

  function strBytes(s, len) {
    var out = [];
    for (var i = 0; i < len; i++) out.push(i < s.length ? (s.charCodeAt(i) & 0x7F) : 0x00);
    return out;
  }
  function encodeRevision(r) {
    // 5 x 7-bit bytes, LSB first.
    var out = [];
    for (var i = 0; i < 5; i++) { out.push(r & 0x7F); r = r >> 7; }
    return out;
  }
  function decodeRevision(bytes) {
    var r = 0;
    for (var i = 0; i < bytes.length; i++) r |= (bytes[i] & 0x7F) << (7 * i);
    return r;
  }

  // ---------------------------------------------------------------------------
  // The API client. Every REST method tries fetch() first. It falls back to the
  // mock ONLY when the backend is unreachable (network failure / forced demo).
  // A real HTTP error (4xx/5xx from the firmware) is propagated to the caller so
  // the UI shows the real failure instead of a faked success.
  // ---------------------------------------------------------------------------
  var api = {
    mock: false,
    _forceMock: false,
    // Admin token attached to write requests (X-GMB-Token). Persisted locally so
    // it survives reloads; set via setAdminToken().
    _adminToken: (typeof localStorage !== 'undefined' && localStorage.getItem('gmbAdminToken')) || '',

    // Force mock mode (used by a "Demo mode" toggle if desired).
    useMock: function (on) { this._forceMock = !!on; if (on) this.mock = true; },

    // Remember an admin token locally (does NOT store it on the device — use
    // setAdminTokenRemote for that).
    setAdminToken: function (t) {
      this._adminToken = t || '';
      if (typeof localStorage !== 'undefined') localStorage.setItem('gmbAdminToken', this._adminToken);
    },
    // Configure the device's admin token (POST /api/auth) and remember it.
    // POST /api/auth/check with a CANDIDATE token: 200 = it is the device's admin
    // token (remember it locally so this browser's writes work), 401 = wrong.
    // Never CHANGES the stored token — a fresh browser that knows the token had no
    // way to authenticate itself before this.
    unlockAdminToken: function (t) {
      var self = this;
      return this._call('/api/auth/check', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'X-GMB-Token': t || '' },
        body: '{}'
      }, function () { return { ok: true }; }).then(function (r) {
        self.setAdminToken(t);
        return r;
      });
    },
    // GET /api/wifi/scan[?start=1] -> { ok, scanning, networks:[{ssid,rssi,secure,
    // channel}] } deduped by SSID, sorted by RSSI. Poll while `scanning` is true.
    wifiScan: function (start) {
      return this._call('/api/wifi/scan' + (start ? '?start=1' : ''), null, function () {
        if (start) { MOCK.scanStartedAt = nowMs(); }
        var elapsed = MOCK.scanStartedAt ? nowMs() - MOCK.scanStartedAt : 1e9;
        if (elapsed < 1500) return { ok: true, scanning: true, networks: [] };
        return { ok: true, scanning: false, networks: [
          { ssid: 'Workshop-24', rssi: -48, secure: true, channel: 6 },
          { ssid: 'FreeCafe', rssi: -61, secure: false, channel: 11 },
          { ssid: 'Neighbor', rssi: -77, secure: true, channel: 1 }
        ] };
      });
    },
    // POST /api/hotspot -> switch to the access point + captive portal now. The
    // web twin of the BOOT-button long-press, for when the station link is gone.
    startHotspot: function () {
      return this._call('/api/hotspot', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}'
      }, function () {
        return { ok: true, note: 'Hotspot starting (mock) — rejoin the device Wi-Fi.' };
      });
    },
    // GET /api/diagnostics -> the runtime telemetry snapshot (uptime, reset reason,
    // heap, MIDI/UDP counters, scheduler latency & jitter, move mix, motion counters,
    // per-board PCA health). Built loop-side, so this never blocks on I2C.
    getDiagnostics: function () {
      return this._call('/api/diagnostics', null, function () { return mockDiagnostics(); });
    },
    // POST /api/midi/source -> { ok }. UDP MIDI source posture: policy
    // "open"|"lockToFirst"|"disabled" and/or unlock:true to forget the locked sender.
    setMidiSource: function (payload) {
      var body = {};
      if (payload.policy) body.policy = payload.policy;
      if (payload.unlock) body.unlock = true;
      return this._call('/api/midi/source', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body)
      }, function () {
        if (body.policy) MOCK.midiSourcePolicy = body.policy;
        if (body.unlock) MOCK.midiSourceLocked = false;
        return { ok: true };
      });
    },
    setAdminTokenRemote: function (t) {
      var self = this;
      return this._call('/api/auth', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ token: t })
      }, function () { return { ok: true }; }).then(function (r) { self.setAdminToken(t); return r; });
    },

    _fetch: function (path, opts) {
      var self = this;
      if (this._forceMock) {
        var fm = new Error('forced-mock'); fm.network = true; return Promise.reject(fm);
      }
      // Attach the admin token to write requests when one is configured.
      if (this._adminToken && opts && opts.method && opts.method !== 'GET') {
        opts.headers = opts.headers || {};
        opts.headers['X-GMB-Token'] = this._adminToken;
      }
      return fetch(path, opts).then(function (r) {
        // Backend replied. Whatever the status, we are NOT offline.
        self.mock = false;
        var ct = r.headers.get('content-type') || '';
        var bodyP = ct.indexOf('application/json') >= 0
          ? r.json().catch(function () { return {}; })
          : r.text();
        if (!r.ok) {
          return bodyP.then(function (body) {
            var e = new Error('HTTP ' + r.status);
            e.httpStatus = r.status;
            e.body = body;             // e.g. { ok:false, issues:[...] }
            throw e;                   // real error — do not mask with the mock
          });
        }
        return bodyP;
      }, function (networkErr) {
        // fetch() itself rejected -> the backend is unreachable.
        var e = new Error('network'); e.network = true; e.cause = networkErr;
        throw e;
      });
    },

    // Wrap a REST call so it falls back to the mock ONLY when offline.
    _call: function (path, opts, mockFn) {
      var self = this;
      return this._fetch(path, opts).catch(function (e) {
        if (self._forceMock || (e && e.network)) { self.mock = true; return mockFn(); }
        throw e;  // propagate real HTTP errors to the caller
      });
    },

    getStatus: function () {
      return this._call('/api/status', null, function () { return sampleStatus(); });
    },
    getProfile: function () {
      return this._call('/api/profile', null, function () { return deepCopy(MOCK.profile); });
    },
    putProfile: function (profile) {
      return this._call('/api/profile', {
        method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(profile)
      }, function () {
        MOCK.profile = deepCopy(profile);
        MOCK.profile.capabilitiesRevision = (MOCK.profile.capabilitiesRevision || 0) + 1;
        return { ok: true, capabilitiesRevision: MOCK.profile.capabilitiesRevision };
      });
    },
    // GET /api/profiles -> { profiles:[{slot,name,used}], startupSlot }.
    getProfiles: function () {
      return this._call('/api/profiles', null, function () { return mockProfilesList(); });
    },
    // POST /api/profiles -> { ok } : save a full profile to a slot (optionally
    // making it the startup slot).
    saveProfileSlot: function (slot, profile, startup) {
      var body = { slot: slot, profile: profile };
      if (startup) body.startup = true;
      return this._call('/api/profiles', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body)
      }, function () {
        MOCK.slots[slot] = deepCopy(profile);
        if (startup) MOCK.startupSlot = slot;
        return { ok: true };
      });
    },
    // POST /api/profiles/load -> { ok } (404 if the slot is empty).
    // NOTE: this ACTIVATES the slot and triggers a re-home on the firmware.
    loadProfileSlot: function (slot) {
      return this._call('/api/profiles/load', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ slot: slot })
      }, function () {
        var stored = MOCK.slots[slot];
        if (!stored) return { ok: false, error: 'slot not found' };
        MOCK.profile = deepCopy(stored);
        return { ok: true };
      });
    },
    // POST /api/profiles/read -> the slot's profile JSON, WITHOUT activating it
    // (no homing / no actuator movement). Used by copy / rename / set-startup.
    readProfileSlot: function (slot) {
      return this._call('/api/profiles/read', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ slot: slot })
      }, function () {
        var stored = MOCK.slots[slot];
        if (!stored) throw Object.assign(new Error('HTTP 404'), { httpStatus: 404 });
        return deepCopy(stored);
      });
    },
    // POST /api/reset -> recover from a panic / E-stop, then re-home.
    resetSystem: function () {
      return this._call('/api/reset', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}'
      }, function () { return { ok: true }; });
    },
    // POST /api/profiles/delete -> { ok }.
    deleteProfileSlot: function (slot) {
      return this._call('/api/profiles/delete', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ slot: slot })
      }, function () {
        MOCK.slots[slot] = null;
        if (MOCK.startupSlot === slot) MOCK.startupSlot = 0;
        return { ok: true };
      });
    },
    getBoard: function (id) {
      return this._call('/api/board/' + id, null, function () {
        return deepCopy(boardById(id) || currentBoard());
      });
    },
    // GET /gmb/descriptor.json -> the GMB v2 descriptor the firmware serves. This
    // is the authoritative view of what a General-Midi-Boop controller receives.
    getDescriptor: function () {
      return this._call('/gmb/descriptor.json', null, function () {
        return GMB.mockDescriptor((global.GMB.state && global.GMB.state.profile) || MOCK.profile);
      });
    },
    // GET /api/boards -> { boards:[{identifier,displayName}] }. The board picker is
    // built from this, so it can only ever offer boards the firmware supports.
    listBoards: function () {
      return this._call('/api/boards', null, function () { return { boards: boardList() }; })
        .then(function (r) { return (r && r.boards) || []; });
    },
    // POST /api/pins/auto. The board is injected HERE rather than left to each
    // caller: the backend assigns against whichever board it is told, and a caller
    // that forgot the field would silently get pins for the wrong one. Two of the
    // three call sites did exactly that.
    autoPins: function (req) {
      var draft = (global.GMB.state && global.GMB.state.profile) || MOCK.profile;
      var body = {};
      Object.keys(req || {}).forEach(function (k) { body[k] = req[k]; });
      if (!body.board) body.board = draft && draft.board && draft.board.profile;
      return this._call('/api/pins/auto', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body)
      }, function () { return mockAutoAssign(body); });
    },
    // POST /api/pins/validate -> { ok, issues:[{field,message,severity}] }.
    // The backend decodes the body as a full Profile and runs its validator, so
    // we send the whole draft profile (not just the pins).
    validatePins: function (profile) {
      return this._call('/api/pins/validate', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(profile)
      }, function () { return mockValidatePins(profile); });
    },
    panic: function () {
      return this._call('/api/panic', { method: 'POST' }, function () {
        return { ok: true, message: 'PANIC executed: drivers disabled, servos neutralised, queue flushed.' };
      });
    },
    // POST /api/test/note -> { ok:true } (200) or { ok:false, error } (409 when
    // the instrument is homing / not ready). The firmware reads only
    // channel/note/velocity/durationMs; the richer payload feeds the mock trace.
    // POST /api/test/note. `ccString` / `ccFret` are the SELECTION CC VALUES a
    // controller would put on the wire; the firmware emits them on the configured
    // CC numbers before the Note On, so the test walks the real selector (mapping,
    // offset, numbering and all) instead of bypassing it. Omit them for a plain
    // automatically-allocated note.
    testNote: function (payload) {
      var wire = { channel: payload.channel | 0, note: payload.note | 0,
        velocity: payload.velocity | 0, durationMs: payload.durationMs || 500 };
      if (payload.ccString !== undefined && payload.ccString !== null)
        wire.ccString = payload.ccString | 0;
      if (payload.ccFret !== undefined && payload.ccFret !== null)
        wire.ccFret = payload.ccFret | 0;
      return this._call('/api/test/note', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(wire)
      }, function () { return mockTestNote(payload); });
    },
    // POST /api/test/servo -> { ok } (409 if not armed). Body: { index, active }.
    testServo: function (payload) {
      var wire = { index: payload.index | 0, active: !!payload.active };
      return this._call('/api/test/servo', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(wire)
      }, function () { return mockTestServo(payload); });
    },
    // GET /api/commands?id=N -> { id, state:"queued"|"succeeded"|"refused"|"unknown" }.
    // Lets a 202-accepted command (e.g. a jog) be followed up for its real outcome.
    commandState: function (id) {
      return this._call('/api/commands?id=' + encodeURIComponent(id), null,
        function () { return { id: id, state: 'succeeded' }; });
    },
    // POST /api/test/jog -> { ok } (409 if not armed). Body: { axis, deltaMm }.
    jog: function (payload) {
      var wire = { axis: payload.axis | 0, deltaMm: Number(payload.deltaMm) || 0 };
      return this._call('/api/test/jog', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(wire)
      }, function () { return mockAxisMove(wire.axis, mockAxisPos(wire.axis) + wire.deltaMm); });
    },
    // POST /api/test/moveto -> { ok } (409 if not armed). Body: { axis, positionMm }.
    // Absolute target from the homing zero, for fret calibration: reaching fret 9
    // by summing jogs folds every rounding error into the value the operator is
    // about to record as ground truth.
    moveTo: function (payload) {
      var wire = { axis: payload.axis | 0, positionMm: Number(payload.positionMm) || 0 };
      return this._call('/api/test/moveto', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(wire)
      }, function () { return mockAxisMove(wire.axis, wire.positionMm); });
    },
    // POST /api/test/endstop -> { ok:true, home:Bool, limit:Bool }. Body: { axis }.
    testEndstop: function (payload) {
      var wire = { axis: payload.axis | 0 };
      return this._call('/api/test/endstop', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(wire)
      }, function () { return mockTestEndstop(payload); });
    },
    // POST /api/wifi -> { ok:true }. Passwords are write-only.
    // POST /api/wifi -> { ok, note, applied }. Passwords are write-only and are
    // only overwritten when non-empty; erasing one needs its explicit clear flag.
    // `apply` reconfigures the radio immediately instead of waiting for a reboot.
    // Send the WHOLE request: this used to forward only the two passwords, so the
    // mode / SSID / hostname / clear / apply controls above it did nothing.
    setWifi: function (payload) {
      var wire = {
        stationPassword: payload.stationPassword || '',
        apPassword: payload.apPassword || '',
        clearStationPassword: !!payload.clearStationPassword,
        clearApPassword: !!payload.clearApPassword,
        apply: !!payload.apply
      };
      // mode is what tells the firmware a link config is present at all; omitting
      // it leaves the stored network untouched (a password-only change).
      if (payload.mode) {
        wire.mode = payload.mode;
        wire.ssid = payload.ssid || '';
        wire.apSsid = payload.apSsid || '';
        wire.hostname = payload.hostname || '';
      }
      return this._call('/api/wifi', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(wire)
      }, function () { return mockSetWifi(wire); });
    },
    // POST /api/sysex/request: build the request bytes for the block, send
    // { bytes:[...] }, then decode the returned response bytes for display.
    sysexRequest: function (kind) {
      var prof = (global.GMB.state && global.GMB.state.profile) || MOCK.profile;
      var t0 = nowMs();
      // Block 8 (notify) is device-emitted — there is no host->device request,
      // so it is built and decoded locally.
      if (kind === 'notify') {
        return Promise.resolve(makeSysexView('notify', null, buildNotifyMessage(prof), t0));
      }
      var req = buildSysexRequest(kind, prof.midi.globalChannel);
      if (!req) return Promise.resolve(makeSysexView(kind, null, [], t0));
      return this._call('/api/sysex/request', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ bytes: req })
      }, function () { return mockSysExBackend({ bytes: req }); }).then(function (res) {
        return makeSysexView(kind, req, (res && res.response) || [], t0);
      });
    },
    getCapabilities: function () {
      return this._call('/api/capabilities', null, function () { return GMB.computeCapabilities(MOCK.profile); });
    }
  };

  function mockTestNote(payload) {
    // Emulate the integrated test tool (selection spec 16) with a step log. The
    // selection CCs are only traced when they were actually sent — a note with no
    // selection is allocated automatically, and the trace has to say so rather
    // than invent a selection step that never happened.
    var p = (global.GMB.state && global.GMB.state.profile) || MOCK.profile;
    var sfs = p.stringFretSelection;
    var hasSel = payload.ccString !== undefined && payload.ccString !== null &&
                 payload.ccFret !== undefined && payload.ccFret !== null;
    var steps = [];
    if (hasSel) {
      // Decode the CC value exactly as the firmware selector does, so the trace
      // shows which PHYSICAL axis a given CC value lands on under the current
      // numbering / offset / order / mapping — that is the thing being tested.
      var axis = GMB.decodeStringCc ? GMB.decodeStringCc(sfs, p, payload.ccString) : -1;
      steps.push({ step: 'CC string received',
                   detail: 'CC' + sfs.string.ccNumber + ' = ' + payload.ccString +
                           (axis >= 0 ? ' -> axis ' + (axis + 1) : ' -> invalid') });
      steps.push({ step: 'CC fret received',
                   detail: 'CC' + sfs.fret.ccNumber + ' = ' + payload.ccFret });
      if (axis < 0) {
        steps.push({ step: 'Selection rejected',
                     detail: 'no such string — falling back to automatic allocation' });
      } else {
        steps.push({ step: 'Selection validated',
                     detail: 'axis ' + (axis + 1) + ', fret ' + payload.ccFret });
        steps.push({ step: 'Axis moving',
                     detail: 'axis ' + (axis + 1) + ' -> fret ' + payload.ccFret });
        steps.push({ step: 'Position reached', detail: 'ok' });
        steps.push({ step: 'Finger pressed',
                     detail: payload.ccFret === 0 ? 'skipped (open string)' : 'ok' });
      }
    } else {
      steps.push({ step: 'Note On received',
                   detail: 'note ' + payload.note + ', no selection CC' });
      steps.push({ step: 'Allocated automatically', detail: 'controller picked the string' });
    }
    steps.push({ step: 'String plucked', detail: 'velocity ' + payload.velocity });
    // Also inject the events into the mock MIDI stream so the monitor shows them.
    injectMidi(payload);
    return { ok: true, accepted: true, steps: steps };
  }

  // Mock servo test (/api/test/servo): matches the firmware contract
  // { ok } for a { index, active } request.
  function mockTestServo(payload) {
    var to = payload.active ? 'active' : 'rest';
    var us = payload.active ? (payload.activeUs || 1800) : (payload.restUs || 1000);
    var where = payload.source === 'gpio'
      ? ('GPIO' + (payload.gpio >= 0 ? payload.gpio : '—'))
      : ('PCA board ' + (payload.pcaBoard || 0) + ', channel ' + (payload.channel || 0));
    return {
      ok: true,
      message: 'Servo "' + (payload.function || 'servo') + '" (' + where + ') driven to ' +
        to + ' (' + us + ' µs).'
    };
  }

  // Mock endstop test (/api/test/endstop): matches the firmware contract
  // { ok:true, home:Bool, limit:Bool } for an { axis } request.
  function mockTestEndstop(payload) {
    return { ok: true, home: Math.random() < 0.25, limit: Math.random() < 0.1 };
  }

  // Mock /api/wifi. It mirrors the firmware's validation rather than always
  // answering ok: a demo that accepts what the device refuses teaches the wrong
  // thing about the form. Secrets are not stored anywhere — only their presence.
  var mockWifiState = { stationPassword: false, apPassword: false };
  // Same shape _fetch() throws for a real HTTP error, so a caller reading
  // e.body.error works identically against the device and against the mock.
  function apiError(status, body) {
    var e = new Error('HTTP ' + status);
    e.httpStatus = status;
    e.body = body;
    return e;
  }
  function mockSetWifi(wire) {
    if (wire.apPassword && (wire.apPassword.length < 8 || wire.apPassword.length > 63))
      return Promise.reject(apiError(422, { error: 'apPassword must be 8-63 characters (WPA2)' }));
    if ((wire.apPassword && wire.clearApPassword) ||
        (wire.stationPassword && wire.clearStationPassword))
      return Promise.reject(apiError(422, { error: 'cannot set and clear the same password' }));
    if (wire.mode !== undefined) {
      if (wire.mode !== 'accessPoint' && wire.mode !== 'station')
        return Promise.reject(apiError(422, { error: 'mode must be "accessPoint" or "station"' }));
      if (!wire.apSsid)
        return Promise.reject(apiError(422, { error: 'apSsid must not be empty' }));
      if (wire.mode === 'station' && !wire.ssid)
        return Promise.reject(apiError(422, { error: 'station mode needs an ssid' }));
    }
    if (wire.stationPassword) mockWifiState.stationPassword = true;
    if (wire.clearStationPassword) mockWifiState.stationPassword = false;
    if (wire.apPassword) mockWifiState.apPassword = true;
    if (wire.clearApPassword) mockWifiState.apPassword = false;
    return { ok: true, applied: !!wire.apply,
             note: wire.apply ? 'applied now (mock)' : 'stored (mock); reboot to apply' };
  }

  // ---------------------------------------------------------------------------
  // WebSocket helpers. Real WS if reachable, otherwise a mock event pump.
  // Consumers use GMB.api.connect('/ws/midi', onMessage) -> returns {close}.
  // ---------------------------------------------------------------------------
  var midiSubscribers = [];
  var statusSubscribers = [];

  api.connectMidi = function (onEvent) {
    return connect('/ws/midi', onEvent, midiSubscribers, startMidiPump);
  };
  api.connectStatus = function (onEvent) {
    return connect('/ws/status', onEvent, statusSubscribers, startStatusPump);
  };

  function connect(path, onEvent, subs, startPump) {
    var url;
    try {
      var proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
      url = proto + '//' + location.host + path;
    } catch (e) { url = null; }
    var handle = { close: function () {}, mock: false };
    if (!url || location.protocol === 'file:') { return mockConnect(onEvent, subs, startPump, handle); }
    try {
      var ws = new WebSocket(url);
      var opened = false;
      ws.onopen = function () { opened = true; api.mock = false; };
      ws.onmessage = function (ev) { try { onEvent(JSON.parse(ev.data)); } catch (e) {} };
      ws.onerror = function () { if (!opened) { ws.close(); mockConnect(onEvent, subs, startPump, handle); } };
      ws.onclose = function () { if (!opened) mockConnect(onEvent, subs, startPump, handle); };
      handle.close = function () { try { ws.close(); } catch (e) {} };
      return handle;
    } catch (e) {
      return mockConnect(onEvent, subs, startPump, handle);
    }
  }

  function mockConnect(onEvent, subs, startPump, handle) {
    api.mock = true;
    handle.mock = true;
    subs.push(onEvent);
    startPump();
    handle.close = function () {
      var i = subs.indexOf(onEvent);
      if (i >= 0) subs.splice(i, 1);
    };
    return handle;
  }

  // Mock MIDI event pump — periodically emits a plausible GMB tablature sequence.
  var midiTimer = null, midiTime = 0;
  function startMidiPump() {
    if (midiTimer) return;
    var seq = 0;
    midiTimer = setInterval(function () {
      if (!midiSubscribers.length) { clearInterval(midiTimer); midiTimer = null; return; }
      var p = MOCK.profile, sfs = p.stringFretSelection;
      var str = 1 + (seq % p.instrument.stringCount);
      var fret = (seq * 2) % (p.strings[str - 1].maxFret + 1);
      var note = p.strings[str - 1].openNote + fret;
      emitMidi({ channel: 1, type: 'cc', cc: sfs.string.ccNumber, value: str,
        interpretation: 'string ' + str });
      emitMidi({ channel: 1, type: 'cc', cc: sfs.fret.ccNumber, value: fret,
        interpretation: 'fret ' + fret });
      emitMidi({ channel: 1, type: 'noteOn', note: note, value: 100,
        interpretation: 'string ' + str + ', fret ' + fret + (fret === 0 ? ' (open)' : '') });
      seq++;
    }, 2500);
  }

  function emitMidi(ev) {
    ev.timeMs = Math.round(midiTime += 1);
    ev.t = Date.now();
    midiSubscribers.forEach(function (fn) { try { fn(ev); } catch (e) {} });
  }

  // Push test-tool events straight into the monitor stream — the CC lines only
  // when selection CCs were really part of the request, so the monitor shows the
  // same traffic the device would have seen and nothing else.
  function injectMidi(payload) {
    var p = (global.GMB.state && global.GMB.state.profile) || MOCK.profile;
    var sfs = p.stringFretSelection;
    var ch = (payload.channel || 0) + 1;
    var axis = -1;
    if (payload.ccString !== undefined && payload.ccString !== null) {
      axis = GMB.decodeStringCc(sfs, p, payload.ccString);
      emitMidi({ channel: ch, type: 'cc', cc: sfs.string.ccNumber, value: payload.ccString,
        interpretation: axis >= 0 ? 'string ' + (axis + 1) : 'invalid string value' });
    }
    if (payload.ccFret !== undefined && payload.ccFret !== null) {
      var fret = GMB.decodeFretCc(sfs, payload.ccFret);
      emitMidi({ channel: ch, type: 'cc', cc: sfs.fret.ccNumber, value: payload.ccFret,
        interpretation: fret >= 0 ? 'fret ' + fret : 'invalid fret value' });
    }
    emitMidi({ channel: ch, type: 'noteOn', note: payload.note, value: payload.velocity,
      interpretation: axis >= 0 ? 'string ' + (axis + 1) : 'auto-allocated' });
  }
  GMB.injectMidi = injectMidi;

  // Mock status pump — small live jitter so the dashboard feels alive.
  var statusTimer = null;
  function startStatusPump() {
    if (statusTimer) return;
    statusTimer = setInterval(function () {
      if (!statusSubscribers.length) { clearInterval(statusTimer); statusTimer = null; return; }
      var st = sampleStatus();
      st.temperatures[0].c = +(33 + Math.random() * 3).toFixed(1);
      st.voltages[0].v = +(23.9 + Math.random() * 0.3).toFixed(2);
      statusSubscribers.forEach(function (fn) { try { fn(st); } catch (e) {} });
    }, 1500);
  }

  // ---------------------------------------------------------------------------
  // Small utilities.
  // ---------------------------------------------------------------------------
  function deepCopy(o) { return JSON.parse(JSON.stringify(o)); }
  GMB.deepCopy = deepCopy;
  function slug(s) { return String(s).toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/(^-|-$)/g, '') || 'profile'; }
  GMB.slug = slug;

  GMB.api = api;
})(window);
