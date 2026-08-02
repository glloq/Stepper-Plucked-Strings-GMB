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
  // ESP32-S3-DevKitC-1 board profile (cahier des charges 11.4 / 11.5).
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

  function buildDevKitC1() {
    var pins = [];
    // Strapping / boot pins — usable but risky, advanced only.
    pins.push(pin(0, { strapping: true, highSpeedOutput: true, preference: 'reserved',
      note: 'BOOT strapping pin — reserved to keep boot reliable.' }));
    pins.push(pin(1, { adc: true, highSpeedOutput: true, preference: 'recommended', note: 'ADC1_CH0.' }));
    pins.push(pin(2, { adc: true, highSpeedOutput: true, preference: 'recommended', note: 'ADC1_CH1.' }));
    pins.push(pin(3, { adc: true, strapping: true, highSpeedOutput: true, preference: 'caution',
      note: 'Strapping pin (JTAG source select) — use with care in advanced mode.' }));
    // Recommended general I/O — the auto-assigner draws STEP/DIR/HOME from here.
    [4, 5, 6, 7].forEach(function (g) {
      pins.push(pin(g, { adc: true, highSpeedOutput: true, preference: 'recommended',
        note: 'Fast, general purpose — recommended for STEP.' }));
    });
    [8, 9, 10, 11].forEach(function (g) {
      pins.push(pin(g, { adc: true, highSpeedOutput: true, preference: 'recommended',
        note: 'General purpose — recommended for DIR.' }));
    });
    [12, 13, 14].forEach(function (g) {
      pins.push(pin(g, { adc: true, highSpeedOutput: true, preference: 'recommended',
        note: 'General purpose — recommended for HOME sensors.' }));
    });
    [15, 16].forEach(function (g) {
      pins.push(pin(g, { adc: true, highSpeedOutput: true, preference: 'recommended',
        note: 'General purpose (ADC2) — recommended for STEP.' }));
    });
    [17, 18].forEach(function (g) {
      pins.push(pin(g, { adc: true, highSpeedOutput: true, preference: 'recommended',
        note: 'General purpose (ADC2) — recommended for DIR.' }));
    });
    // Native USB — reserved by default (cahier des charges 8.3 / 11.3).
    pins.push(pin(19, { usb: true, preference: 'reserved',
      note: 'USB-JTAG / native USB (D-). Reserved for future USB MIDI.' }));
    pins.push(pin(20, { usb: true, preference: 'reserved',
      note: 'USB-JTAG / native USB (D+). Reserved for future USB MIDI.' }));
    pins.push(pin(21, { highSpeedOutput: true, preference: 'recommended',
      note: 'General purpose — recommended for HOME sensor.' }));
    // GPIO22..25 do not exist on the ESP32-S3.
    // SPI flash — never usable on the DevKitC-1 module.
    [26, 27, 28, 29, 30, 31, 32].forEach(function (g) {
      pins.push(pin(g, { preference: 'reserved', reserved: true,
        note: 'Connected to on-module SPI flash — not available.' }));
    });
    // Variant-dependent memory pins.
    pins.push(pin(33, { highSpeedOutput: true, preference: 'caution',
      note: 'May be used by octal PSRAM on some variants — verify your module.' }));
    pins.push(pin(34, { highSpeedOutput: true, preference: 'caution',
      note: 'May be used by octal PSRAM on some variants — verify your module.' }));
    [35, 36, 37].forEach(function (g) {
      pins.push(pin(g, { preference: 'reserved', reserved: true,
        note: 'Flash/PSRAM on octal variants — reserved unless the variant frees it.' }));
    });
    [38, 39].forEach(function (g) {
      pins.push(pin(g, { highSpeedOutput: true, preference: 'recommended',
        note: 'General purpose — recommended for HOME sensor.' }));
    });
    pins.push(pin(40, { preference: 'recommended', note: 'Recommended I2C SDA (PCA9685).' }));
    pins.push(pin(41, { preference: 'recommended', note: 'Recommended I2C SCL (PCA9685).' }));
    pins.push(pin(42, { preference: 'recommended', note: 'Recommended global driver ENABLE.' }));
    // Main UART — programming / diagnostics.
    pins.push(pin(43, { onboardPeripheral: true, preference: 'reserved',
      note: 'U0TXD — programming & diagnostic UART. Reserved.' }));
    pins.push(pin(44, { onboardPeripheral: true, preference: 'reserved',
      note: 'U0RXD — programming & diagnostic UART. Reserved.' }));
    pins.push(pin(45, { strapping: true, preference: 'caution',
      note: 'Strapping pin (VDD_SPI voltage) — advanced use only.' }));
    pins.push(pin(46, { strapping: true, preference: 'caution',
      note: 'Strapping pin — advanced use only.' }));
    pins.push(pin(47, { highSpeedOutput: true, preference: 'recommended',
      note: 'Recommended PCA9685 /OE safety line.' }));
    pins.push(pin(48, { onboardPeripheral: true, preference: 'reserved',
      note: 'On-board RGB LED (WS2812). Reserved.' }));
    return {
      identifier: 'esp32-s3-devkitc-1',
      displayName: 'ESP32-S3-DevKitC-1',
      pins: pins
    };
  }

  // ---------------------------------------------------------------------------
  // Default recommended pin assignment (cahier des charges 11.5). Signal names
  // match firmware PinAssignment.signal ("STEP1", "HOME3", "SDA"...).
  // ---------------------------------------------------------------------------
  var RECOMMENDED = {
    STEP: [4, 5, 6, 7, 15, 16],
    DIR: [17, 18, 8, 9, 10, 11],
    HOME: [12, 13, 14, 21, 38, 39],
    SDA: 40, SCL: 41, ENABLE: 42, SERVO_OE: 47
  };
  GMB.RECOMMENDED = RECOMMENDED;

  // Which capability a signal kind needs (mirrors BoardProfile::candidatesFor).
  var SIGNAL_KIND = {
    step: 'step', dir: 'dir', enable: 'enable', home: 'home', limit: 'limit',
    diag: 'diag', sda: 'i2cSda', scl: 'i2cScl', servoOe: 'servoOe', servo: 'servo'
  };
  GMB.SIGNAL_KIND = SIGNAL_KIND;

  // Can a pin (statically) carry a given signal kind? (cahier des charges 11.3)
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
  // Sample profile — a 4-string GCEA ukulele (reentrant tuning G4 C4 E4 A4).
  // Matches the JSON schema in the project brief exactly.
  // ---------------------------------------------------------------------------
  function ukuleleString(openNote) {
    return {
      openNote: openNote, maxFret: 12, scaleLengthMm: 330,
      transmission: 'beltGt2', stepsPerRevolution: 200, microsteps: 16,
      pulleyTeeth: 20, beltPitchMm: 2, leadPerRevolutionMm: 8, customStepsPerMm: 80,
      invertDirection: false, minPositionMm: 0, maxPositionMm: 300,
      maxSpeedMmS: 200, maxAccelMmS2: 2000, calibratedFretMm: [],
      homing: {
        direction: -1, fastSpeedMmS: 40, slowSpeedMmS: 5, backoffMm: 3, offsetMm: 0,
        timeoutMs: 8000, maxSearchMm: 500, sensorActiveHigh: true
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
      pcaBoard: opts.pcaBoard || 0,   // 0..3 (0x40..0x43)
      channel: opts.channel === undefined ? 0 : opts.channel, // 0..15 (source == pca)
      gpio: opts.gpio === undefined ? -1 : opts.gpio,         // ESP32 GPIO (source == gpio)
      pulseMinUs: opts.pulseMinUs || 500,
      pulseMaxUs: opts.pulseMaxUs || 2500,
      restUs: opts.restUs || 1000,
      activeUs: opts.activeUs || 1800,
      inverted: !!opts.inverted,
      travelMs: opts.travelMs || 120,
      settleMs: opts.settleMs || 30,
      disableAtRest: opts.disableAtRest !== false
    };
  }
  GMB.servoDefaults = servo;

  // Theoretical fret position (cahier des charges 14.2): scale·(1−2^(−fret/12)).
  GMB.fretTheoreticalMm = function (s, fret) {
    return (s.scaleLengthMm || 0) * (1 - Math.pow(2, -fret / 12));
  };

  function sampleProfile() {
    return {
      project: 'Stepper-Plucked-Strings-GMB', profileVersion: 1, capabilitiesRevision: 7,
      instrument: {
        name: 'Ukulele GCEA', description: '4-string soprano ukulele',
        stringCount: 4, type: 'ukulele', gmProgram: 24, typeId: 4,
        capo: 0, transpose: 0, pluckMode: 'individual'
      },
      board: { profile: 'esp32-s3-devkitc-1', reserveUsb: true, automaticPinAssignment: true },
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
      network: {
        mode: 'accessPoint', ssid: '', hostname: 'gmb-instrument',
        apSsid: 'Stepper-Plucked-Strings-GMB', staticIp: false
      },
      midi: {
        globalChannel: 0, omni: false, transpose: 0, chordWindowMs: 3,
        velocityCurve: 'linear', sustainPedal: true
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
      // Ukulele GCEA: physical order low->high used by GMB = G4(67) C4(60) E4(64) A4(69)
      strings: [ukuleleString(67), ukuleleString(60), ukuleleString(64), ukuleleString(69)],
      // A representative mix: one finger + one pluck per string on PCA board 0
      // (channels 0–3 fingers, 6–9 plucks — the recommended layout), plus one
      // shared strum driven directly from a free ESP32 GPIO (no PCA needed).
      servos: [
        servo('finger', 0, { channel: 0 }),
        servo('finger', 1, { channel: 1 }),
        servo('finger', 2, { channel: 2 }),
        servo('finger', 3, { channel: 3 }),
        servo('pluck', 0, { channel: 6, activeUs: 1700, travelMs: 90, settleMs: 20 }),
        servo('pluck', 1, { channel: 7, activeUs: 1700, travelMs: 90, settleMs: 20 }),
        servo('pluck', 2, { channel: 8, activeUs: 1700, travelMs: 90, settleMs: 20 }),
        servo('pluck', 3, { channel: 9, activeUs: 1700, travelMs: 90, settleMs: 20 }),
        servo('sharedStrum', -1, { source: 'gpio', gpio: 2, activeUs: 1700, travelMs: 90, settleMs: 20 })
      ]
    };
  }
  GMB.sampleProfile = sampleProfile;

  var NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
  GMB.noteName = function (n) {
    if (n === null || n === undefined || n < 0) return '--';
    return NOTE_NAMES[n % 12] + (Math.floor(n / 12) - 1);
  };

  // Derive read-only capabilities from a profile (SysEx spec 5 / 6 / 17).
  GMB.computeCapabilities = function (p) {
    var strings = p.strings || [];
    var notesSet = {};
    var min = 127, max = 0;
    strings.forEach(function (s) {
      var lo = s.openNote + (p.instrument.capo || 0);
      var hi = lo + (s.maxFret || 0);
      for (var n = lo; n <= hi; n++) { notesSet[n] = true; if (n < min) min = n; if (n > max) max = n; }
    });
    var allContinuous = true;
    for (var n2 = min; n2 <= max; n2++) if (!notesSet[n2]) { allContinuous = false; break; }
    var sfs = p.stringFretSelection;
    var tuning = strings.map(function (s) { return s.openNote; });
    return {
      strings: p.instrument.stringCount,
      frets: Math.max.apply(null, strings.map(function (s) { return s.maxFret; }).concat([0])),
      noteMin: min, noteMax: max, noteMode: allContinuous ? 0 : 1,
      polyphony: strings.length,
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
  // Live status (dashboard, cahier des charges 19). Mock evolves over time.
  // ---------------------------------------------------------------------------
  function sampleStatus() {
    var p = MOCK.profile;
    var caps = GMB.computeCapabilities(p);
    return {
      state: 'READY',
      wifi: { mode: p.network.mode, ssid: p.network.mode === 'station' ? p.network.ssid : p.network.apSsid,
        ip: p.network.mode === 'station' ? '192.168.1.42' : '192.168.4.1', rssi: -54, connected: true },
      midiSource: 'WebSocket MIDI (Wi-Fi)',
      activeProfile: p.instrument.name,
      stringsReady: p.instrument.stringCount, stringsTotal: p.instrument.stringCount,
      notesPlaying: 0,
      faults: [],
      capabilitiesRevision: p.capabilitiesRevision,
      temperatures: [{ name: 'Driver board', c: 34.2 }],
      voltages: [{ name: '24V motor', v: 24.1 }, { name: '5V servo', v: 5.02 }],
      strings: p.strings.map(function (s, i) {
        return {
          index: i, state: 'IDLE',
          note: null, fret: null,
          positionMm: 0, targetMm: 0, distanceMm: 0,
          home: true, limit: false,
          finger: 'up', plectrum: 'rest', lastFault: 'none',
          openNote: s.openNote
        };
      })
    };
  }

  // ---------------------------------------------------------------------------
  // Mock store.
  // ---------------------------------------------------------------------------
  var MOCK = {
    board: buildDevKitC1(),
    profile: sampleProfile(),
    profiles: [
      { id: 'ukulele-gcea', name: 'Ukulele GCEA', stringCount: 4, type: 'ukulele', startup: true },
      { id: 'guitar-eadgbe', name: 'Guitar Standard', stringCount: 6, type: 'guitar', startup: false },
      { id: 'bass-eadg', name: 'Bass EADG', stringCount: 4, type: 'bass', startup: false }
    ]
  };
  GMB.mockBoard = function () { return MOCK.board; };

  // ---------------------------------------------------------------------------
  // Mock auto-assign — reproduces the recommended profile, respecting the
  // requested string count and reserved USB pins (mirrors PinManager::autoAssign).
  // ---------------------------------------------------------------------------
  function mockAutoAssign(req) {
    var n = req.stringCount || MOCK.profile.instrument.stringCount;
    var pins = [];
    for (var i = 0; i < n; i++) {
      pins.push({ signal: 'STEP' + (i + 1), kind: 'step', gpio: RECOMMENDED.STEP[i] });
      pins.push({ signal: 'DIR' + (i + 1), kind: 'dir', gpio: RECOMMENDED.DIR[i] });
      pins.push({ signal: 'HOME' + (i + 1), kind: 'home', gpio: RECOMMENDED.HOME[i] });
    }
    if (req.useI2cServos !== false) {
      pins.push({ signal: 'SDA', kind: 'sda', gpio: RECOMMENDED.SDA });
      pins.push({ signal: 'SCL', kind: 'scl', gpio: RECOMMENDED.SCL });
    }
    if (req.globalEnable !== false) pins.push({ signal: 'ENABLE', kind: 'enable', gpio: RECOMMENDED.ENABLE });
    if (req.servoSafetyOe !== false) pins.push({ signal: 'SERVO_OE', kind: 'servoOe', gpio: RECOMMENDED.SERVO_OE });
    return { pins: pins, errors: [] };
  }

  // Mock validation (cahier des charges 11.6). Returns { errors: [PinError] }.
  function mockValidatePins(pins, reserveUsb) {
    var errors = [];
    var byGpio = {};
    pins.forEach(function (a) {
      if (a.gpio < 0) return;
      var cap = MOCK.board.pins.filter(function (p) { return p.gpio === a.gpio; })[0];
      // Duplicate use.
      if (byGpio[a.gpio]) {
        errors.push({ signal: a.signal, gpio: a.gpio,
          reason: 'GPIO ' + a.gpio + ' is already used by ' + byGpio[a.gpio] + '.',
          suggestion: suggest(a.kind, pins), conflictWith: byGpio[a.gpio] });
      } else {
        byGpio[a.gpio] = a.signal;
      }
      // Reserved / USB / capability.
      if (!cap) {
        errors.push({ signal: a.signal, gpio: a.gpio,
          reason: 'GPIO ' + a.gpio + ' does not exist on this board.',
          suggestion: suggest(a.kind, pins), conflictWith: '' });
      } else if (reserveUsb && cap.usb) {
        errors.push({ signal: a.signal, gpio: a.gpio,
          reason: 'GPIO ' + a.gpio + ' is reserved for future native USB.',
          suggestion: suggest(a.kind, pins), conflictWith: 'USB (reserved)' });
      } else if (cap.preference === 'reserved') {
        errors.push({ signal: a.signal, gpio: a.gpio,
          reason: cap.note || ('GPIO ' + a.gpio + ' is reserved.'),
          suggestion: suggest(a.kind, pins), conflictWith: '' });
      } else if (!GMB.pinSupports(cap, SIGNAL_KIND[a.kind] || 'generic')) {
        errors.push({ signal: a.signal, gpio: a.gpio,
          reason: 'GPIO ' + a.gpio + ' is not compatible with a ' + a.kind.toUpperCase() + ' signal.',
          suggestion: suggest(a.kind, pins), conflictWith: '' });
      }
    });
    return { errors: errors };
  }

  function suggest(kind, pins) {
    var used = {};
    pins.forEach(function (a) { used[a.gpio] = true; });
    var wantKind = SIGNAL_KIND[kind] || 'generic';
    var free = MOCK.board.pins.filter(function (p) {
      return !used[p.gpio] && p.preference === 'recommended' && GMB.pinSupports(p, wantKind);
    });
    return free.length ? ('Try GPIO ' + free[0].gpio + '.') : 'No free recommended pin — free one up first.';
  }

  // ---------------------------------------------------------------------------
  // SysEx mock (Communication ... SysEx spec). Builds byte arrays + decoded view.
  // ---------------------------------------------------------------------------
  var HEADER = [0xF0, 0x7D, 0x00];
  function hex(bytes) { return bytes.map(function (b) { return ('0' + b.toString(16)).slice(-2).toUpperCase(); }).join(' '); }
  GMB.hex = hex;

  function mockSysEx(kind) {
    var p = MOCK.profile, caps = GMB.computeCapabilities(p);
    var ch = p.midi.globalChannel;
    var req, resp, decoded;
    var t0 = performance.now();
    switch (kind) {
      case 'identity':
        req = HEADER.concat([0x01, 0x00, 0xF7]);
        resp = HEADER.concat([0x01, 0x01, 0x01]) // version
          .concat([0x01, 0x02, 0x03, 0x04, 0x05]) // device id[5]
          .concat(strBytes(p.instrument.name, 32))
          .concat([0x01, 0x00, 0x00]) // firmware 1.0.0
          .concat([0x38, 0xF7]); // features (blocks 5/6/7)
        decoded = { Version: 1, 'Device id': '01 02 03 04 05', Name: p.instrument.name,
          Firmware: '1.0.0', Features: '0x38 (descriptor+capabilities+stringConfig)' };
        break;
      case 'descriptor':
        req = HEADER.concat([0x05, 0x00, 0xF7]);
        resp = HEADER.concat([0x05, 0x01, 0x01, 0x01, ch, p.instrument.gmProgram, p.instrument.typeId, 0xF7]);
        decoded = { Channel: ch + 1, 'GM program': p.instrument.gmProgram, 'Type id': '0x0' + p.instrument.typeId.toString(16) };
        break;
      case 'capabilities':
        req = HEADER.concat([0x06, 0x00, ch, 0xF7]);
        resp = HEADER.concat([0x06, 0x01, 0x01, ch, p.instrument.gmProgram, p.instrument.typeId,
          0x00, caps.noteMode, caps.noteMin, caps.noteMax, caps.polyphony, 0x00,
          caps.supportedCc.length]).concat(caps.supportedCc)
          .concat([p.instrument.name.length]).concat(strBytes(p.instrument.name, p.instrument.name.length))
          .concat([0xF7]);
        decoded = { Channel: ch + 1, Type: p.instrument.type,
          Range: GMB.noteName(caps.noteMin) + ' .. ' + GMB.noteName(caps.noteMax),
          'Note mode': caps.noteMode === 0 ? 'continuous' : 'discrete',
          Polyphony: caps.polyphony, CC: caps.supportedCc.join(', ') };
        break;
      case 'stringConfig':
        req = HEADER.concat([0x07, 0x00, ch, 0xF7]);
        resp = HEADER.concat([0x07, 0x01, 0x01, ch, caps.strings, caps.frets, 0x00, caps.capo,
          caps.ccActive, caps.ccString, caps.ccFret]).concat(caps.tuning).concat([0xF7]);
        decoded = { Channel: ch + 1, Strings: caps.strings, Frets: caps.frets, Fretless: 'no',
          Capo: caps.capo, 'CC string': caps.ccString, 'CC fret': caps.ccFret,
          Tuning: caps.tuningNames.join(' ') };
        break;
      case 'notify':
        req = null;
        resp = HEADER.concat([0x08, 0x02, 0x01, ch]).concat(encodeRevision(caps.revision)).concat([0x0C, 0xF7]);
        decoded = { Channel: ch + 1, Revision: caps.revision, Flags: '0x0C (capabilities+stringConfig changed)' };
        break;
      default:
        req = null; resp = []; decoded = {};
    }
    var valid = resp.every(function (b) { return b === 0xF0 || b === 0xF7 || (b >= 0 && b <= 0x7F); });
    return {
      kind: kind, request: req ? hex(req) : '(spontaneous notification)',
      response: hex(resp), decoded: decoded, valid: valid,
      length: resp.length, durationMs: +(performance.now() - t0 + 1.2).toFixed(2), error: ''
    };
  }

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
  GMB.mockSysEx = mockSysEx;

  // ---------------------------------------------------------------------------
  // The API client. Every REST method tries fetch() first and, on network
  // failure, serves the mock. `GMB.api.mock` is set true once a fetch fails.
  // ---------------------------------------------------------------------------
  var api = {
    mock: false,
    _forceMock: false,

    // Force mock mode (used by a "Demo mode" toggle if desired).
    useMock: function (on) { this._forceMock = !!on; if (on) this.mock = true; },

    _fetch: function (path, opts) {
      var self = this;
      if (this._forceMock) return Promise.reject(new Error('forced-mock'));
      return fetch(path, opts).then(function (r) {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        self.mock = false;
        var ct = r.headers.get('content-type') || '';
        return ct.indexOf('application/json') >= 0 ? r.json() : r.text();
      }).catch(function (e) { self.mock = true; throw e; });
    },

    // Wrap a REST call so a failure falls back to a mock producer.
    _call: function (path, opts, mockFn) {
      return this._fetch(path, opts).catch(function () { return mockFn(); });
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
    getProfiles: function () {
      return this._call('/api/profiles', null, function () { return deepCopy(MOCK.profiles); });
    },
    postProfiles: function (action, payload) {
      return this._call('/api/profiles', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action: action, payload: payload })
      }, function () { return mockProfilesAction(action, payload); });
    },
    getBoard: function (id) {
      return this._call('/api/board/' + id, null, function () { return deepCopy(MOCK.board); });
    },
    autoPins: function (req) {
      return this._call('/api/pins/auto', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(req)
      }, function () { return mockAutoAssign(req); });
    },
    validatePins: function (pins, reserveUsb) {
      return this._call('/api/pins/validate', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ pins: pins, reserveUsb: reserveUsb })
      }, function () { return mockValidatePins(pins, reserveUsb); });
    },
    panic: function () {
      return this._call('/api/panic', { method: 'POST' }, function () {
        return { ok: true, message: 'PANIC executed: drivers disabled, servos neutralised, queue flushed.' };
      });
    },
    testNote: function (payload) {
      return this._call('/api/test/note', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload)
      }, function () { return mockTestNote(payload); });
    },
    testServo: function (payload) {
      return this._call('/api/test/servo', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload)
      }, function () { return mockTestServo(payload); });
    },
    testEndstop: function (payload) {
      return this._call('/api/test/endstop', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload)
      }, function () { return mockTestEndstop(payload); });
    },
    sysexRequest: function (kind) {
      return this._call('/api/sysex/request', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ request: kind })
      }, function () { return mockSysEx(kind); });
    },
    getCapabilities: function () {
      return this._call('/api/capabilities', null, function () { return GMB.computeCapabilities(MOCK.profile); });
    }
  };

  function mockProfilesAction(action, payload) {
    var list = MOCK.profiles;
    switch (action) {
      case 'create':
        list.push({ id: slug(payload.name), name: payload.name, stringCount: payload.stringCount || 4,
          type: payload.type || 'ukulele', startup: false });
        break;
      case 'copy':
        var src = list.filter(function (x) { return x.id === payload.id; })[0];
        if (src) list.push({ id: slug(payload.name), name: payload.name,
          stringCount: src.stringCount, type: src.type, startup: false });
        break;
      case 'rename':
        list.forEach(function (x) { if (x.id === payload.id) x.name = payload.name; });
        break;
      case 'delete':
        MOCK.profiles = list.filter(function (x) { return x.id !== payload.id; });
        break;
      case 'setStartup':
        list.forEach(function (x) { x.startup = (x.id === payload.id); });
        break;
    }
    return deepCopy(MOCK.profiles);
  }

  function mockTestNote(payload) {
    // Emulate the integrated test tool (selection spec 16) with a step log.
    var steps = [
      { step: 'CC string received', detail: 'CC' + payload.stringCc + ' = ' + payload.string },
      { step: 'CC fret received', detail: 'CC' + payload.fretCc + ' = ' + payload.fret },
      { step: 'Selection validated', detail: 'string ' + payload.string + ', fret ' + payload.fret },
      { step: 'Axis moving', detail: 'string ' + payload.string + ' -> fret ' + payload.fret },
      { step: 'Position reached', detail: 'ok' },
      { step: 'Finger pressed', detail: payload.fret === 0 ? 'skipped (open string)' : 'ok' },
      { step: 'String plucked', detail: 'velocity ' + payload.velocity }
    ];
    // Also inject the events into the mock MIDI stream so the monitor shows them.
    injectMidi(payload);
    return { ok: true, steps: steps };
  }

  // Mock servo test (/api/test/servo): drive a servo to rest or active and
  // report the pulse width used and its wiring (PCA board+channel or GPIO).
  function mockTestServo(payload) {
    var to = payload.to === 'rest' ? 'rest' : 'active';
    var us = to === 'rest' ? (payload.restUs || 1000) : (payload.activeUs || 1800);
    var where = payload.source === 'gpio'
      ? ('GPIO' + (payload.gpio >= 0 ? payload.gpio : '—'))
      : ('PCA board ' + (payload.pcaBoard || 0) + ', channel ' + (payload.channel || 0));
    return {
      ok: true, function: payload.function || 'servo', source: payload.source || 'pca',
      target: to, pulseUs: us, where: where,
      message: 'Servo "' + (payload.function || 'servo') + '" (' + where + ') driven to ' +
        to + ' (' + us + ' µs).'
    };
  }

  // Mock endstop test (/api/test/endstop): a live-ish HOME/LIMIT readout. HOME is
  // usually released (idle); the reported electrical level honours sensorActiveHigh.
  function mockTestEndstop(payload) {
    function read(gpio, activeChance) {
      var active = Math.random() < activeChance;
      var high = payload.sensorActiveHigh === false ? !active : active;
      return { gpio: gpio, level: high ? 'HIGH' : 'LOW', active: active };
    }
    var res = { ok: true, string: payload.string };
    res.home = (payload.homeGpio !== undefined && payload.homeGpio >= 0)
      ? read(payload.homeGpio, 0.25)
      : { gpio: -1, level: '—', active: false, unassigned: true };
    if (payload.limitGpio !== undefined && payload.limitGpio >= 0) {
      res.limit = read(payload.limitGpio, 0.1);
    }
    return res;
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

  // Push test-tool events straight into the monitor stream.
  function injectMidi(payload) {
    emitMidi({ channel: (payload.channel || 0) + 1, type: 'cc', cc: payload.stringCc,
      value: payload.string, interpretation: 'string ' + payload.string });
    emitMidi({ channel: (payload.channel || 0) + 1, type: 'cc', cc: payload.fretCc,
      value: payload.fret, interpretation: 'fret ' + payload.fret });
    emitMidi({ channel: (payload.channel || 0) + 1, type: 'noteOn', note: payload.note,
      value: payload.velocity, interpretation: 'string ' + payload.string + ', fret ' + payload.fret });
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
