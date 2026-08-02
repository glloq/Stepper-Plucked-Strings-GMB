/*
 * wizard.js — first-configuration assistant (cahier des charges section 10).
 *
 * Nine steps: Identification -> Board choice -> Automatic pin assignment ->
 * Mechanics per string -> Homing -> Servo calibration -> Note calibration ->
 * Test -> Validation. Honours the Simplified / Advanced toggle (9.2): advanced
 * mode exposes the detailed per-axis parameters, simplified mode keeps the
 * recommended defaults and hides fine tuning.
 */
(function (global) {
  'use strict';
  var GMB = global.GMB, h = GMB.h;

  var STEPS = [
    'Identification', 'Board', 'Pins', 'Mechanics', 'Homing',
    'Servos', 'Notes', 'Test', 'Validation'
  ];
  var step = 0;

  var TUNINGS = {
    ukulele: { notes: [67, 60, 64, 69], maxFret: 12 },       // G C E A
    guitar: { notes: [40, 45, 50, 55, 59, 64], maxFret: 20 }, // E A D G B E
    bass: { notes: [28, 33, 38, 43], maxFret: 20 },           // E A D G
    mandolin: { notes: [55, 62, 69, 76], maxFret: 18 },
    banjo: { notes: [62, 67, 71, 62], maxFret: 22 }
  };
  var GM_PROGRAM = { ukulele: 24, guitar: 24, bass: 33, mandolin: 25, banjo: 105 };
  var TYPE_ID = { ukulele: 0x04, guitar: 0x04, bass: 0x05, mandolin: 0x04, banjo: 0x04 };

  function render(host) {
    host.appendChild(h('div.card.wizard-card', [
      h('div.stepper', STEPS.map(function (label, i) {
        return h('button.step' + (i === step ? '.active' : '') + (i < step ? '.done' : ''),
          { onclick: function () { goto(i); } },
          [h('span.step-num', String(i + 1)), h('span.step-label', label)]);
      })),
      h('div#wizard-body.wizard-body'),
      h('div.wizard-nav', [
        GMB.button('Back', function () { goto(step - 1); }, 'ghost'),
        h('span.spacer'),
        h('span.muted', 'Step ' + (step + 1) + ' of ' + STEPS.length),
        h('span.spacer'),
        step < STEPS.length - 1
          ? GMB.button('Next', function () { goto(step + 1); }, 'primary')
          : GMB.button('Finish & save', function () { GMB.saveProfile(); }, 'primary')
      ])
    ]));
    drawStep();
  }

  function goto(i) {
    if (i < 0 || i >= STEPS.length) return;
    step = i;
    GMB.render();
  }

  function drawStep() {
    var body = document.getElementById('wizard-body');
    if (!body) return;
    body.innerHTML = '';
    ([stepIdentification, stepBoard, stepPins, stepMechanics, stepHoming,
      stepServos, stepNotes, stepTest, stepValidation][step])(body);
  }

  // ---- Step 1: Identification ----------------------------------------------
  function stepIdentification(body) {
    var p = GMB.state.profile, inst = p.instrument;
    body.appendChild(h('h3', 'Identify the instrument'));
    body.appendChild(h('div.form-grid', [
      GMB.field('Instrument name', GMB.input(inst, 'name')),
      GMB.field('Description (optional)', GMB.input(inst, 'description')),
      GMB.field('Instrument type', GMB.input(inst, 'type', {
        type: 'select', options: Object.keys(TUNINGS).concat(['custom']),
        onChange: applyType
      })),
      GMB.field('Number of strings (1–6)', GMB.input(inst, 'stringCount', {
        type: 'number', min: 1, max: 6, onChange: function (v) { setStringCount(v); }
      })),
      GMB.field('Max frets', GMB.input(strings0(), 'maxFret', { type: 'number', min: 0, max: 30 })),
      GMB.field('GM program', GMB.input(inst, 'gmProgram', { type: 'number', min: 0, max: 127 }))
    ]));
    body.appendChild(h('p.muted', 'A suggested tuning is applied when you pick a type; every value stays editable in the Notes step.'));
  }

  function strings0() { return GMB.state.profile.strings[0] || {}; }

  function applyType(type) {
    var t = TUNINGS[type];
    var p = GMB.state.profile;
    if (t) {
      p.instrument.stringCount = t.notes.length;
      p.instrument.gmProgram = GM_PROGRAM[type] || 24;
      p.instrument.typeId = TYPE_ID[type] || 0x04;
      setStringCount(t.notes.length);
      p.strings.forEach(function (s, i) { s.openNote = t.notes[i]; s.maxFret = t.maxFret; });
      p.stringFretSelection.string.maximum = t.notes.length;
      p.stringFretSelection.fret.maximum = t.maxFret;
    }
    GMB.markDirty();
    drawStep();
  }

  // Grow/shrink the strings/homing arrays to n, cloning defaults.
  function setStringCount(n) {
    n = Math.max(1, Math.min(6, Number(n) || 1));
    var p = GMB.state.profile;
    p.instrument.stringCount = n;
    while (p.strings.length < n) p.strings.push(GMB.deepCopy(p.strings[p.strings.length - 1] || defaultString()));
    p.strings.length = n;
    // Keep the string-CC mapping identity-sized.
    p.stringFretSelection.string.mapping = [];
    for (var i = 0; i < n; i++) p.stringFretSelection.string.mapping.push(i);
    p.stringFretSelection.string.maximum = n;
    GMB.markDirty();
  }

  function defaultString() {
    return {
      openNote: 60, maxFret: 12, scaleLengthMm: 330, transmission: 'beltGt2',
      stepsPerRevolution: 200, microsteps: 16, pulleyTeeth: 20, beltPitchMm: 2,
      leadPerRevolutionMm: 8, customStepsPerMm: 80, invertDirection: false,
      minPositionMm: 0, maxPositionMm: 300, maxSpeedMmS: 200, maxAccelMmS2: 2000,
      calibratedFretMm: [], homing: { direction: -1, fastSpeedMmS: 40, slowSpeedMmS: 5,
        backoffMm: 3, offsetMm: 0, timeoutMs: 8000, maxSearchMm: 500, sensorActiveHigh: true }
    };
  }

  // ---- Step 2: Board --------------------------------------------------------
  function stepBoard(body) {
    var p = GMB.state.profile;
    body.appendChild(h('h3', 'Choose the controller board'));
    body.appendChild(h('div.form-grid', [
      GMB.field('Board model', GMB.input(p.board, 'profile', {
        type: 'select', options: [{ value: 'esp32-s3-devkitc-1', label: 'ESP32-S3-DevKitC-1' }]
      })),
      GMB.field('Reserve GPIO19/20 for future USB', GMB.input(p.board, 'reserveUsb', { type: 'checkbox' })),
      GMB.field('Automatic pin assignment', GMB.input(p.board, 'automaticPinAssignment', { type: 'checkbox' }))
    ]));
    body.appendChild(h('div.note-box', [
      h('strong', 'Board notes:'),
      h('ul', [
        h('li', 'GPIO0/3/45/46 are strapping pins.'),
        h('li', 'GPIO19/20 are the native USB-JTAG pins (kept free by default).'),
        h('li', 'GPIO26–32 drive the on-module SPI flash; 35–37 may serve PSRAM on octal variants.'),
        h('li', 'GPIO43/44 are the programming/diagnostic UART; GPIO48 is the on-board RGB LED.')
      ])
    ]));
  }

  // ---- Step 3: Automatic pins ----------------------------------------------
  function stepPins(body) {
    body.appendChild(h('h3', 'Automatic pin assignment'));
    body.appendChild(h('p', 'Let the system pick a conflict-free set of GPIOs for the current string count.'));
    body.appendChild(h('div.toolbar', [
      GMB.button('Assign automatically', function () {
        var p = GMB.state.profile;
        GMB.api.autoPins({ stringCount: p.instrument.stringCount, reserveUsb: p.board.reserveUsb })
          .then(function (res) { p.pins = res.pins; GMB.markDirty(); drawStep(); GMB.toast('Pins assigned.', 'ok'); });
      }, 'primary'),
      GMB.button('Open full pin editor', function () { GMB.navigate('pins'); }, 'ghost')
    ]));
    var tbl = h('table.mini-table', [
      h('thead', h('tr', [h('th', 'Signal'), h('th', 'Kind'), h('th', 'GPIO')])),
      h('tbody', GMB.state.profile.pins.map(function (a) {
        return h('tr', [h('td', a.signal), h('td', a.kind), h('td', a.gpio < 0 ? '—' : 'GPIO' + a.gpio)]);
      }))
    ]);
    body.appendChild(h('div.table-wrap', tbl));
  }

  // ---- Step 4: Mechanics ----------------------------------------------------
  function stepMechanics(body) {
    body.appendChild(h('h3', 'Mechanics per string'));
    GMB.state.profile.strings.forEach(function (s, i) {
      var spm = stepsPerMm(s);
      var basic = [
        GMB.field('Scale length (mm)', GMB.input(s, 'scaleLengthMm', { type: 'number', onChange: function () { drawStep(); } })),
        GMB.field('Transmission', GMB.input(s, 'transmission', {
          type: 'select', options: [{ value: 'beltGt2', label: 'GT2 belt' }, { value: 'screw', label: 'Screw' }, { value: 'custom', label: 'Custom' }],
          onChange: function () { drawStep(); }
        })),
        GMB.field('Invert direction', GMB.input(s, 'invertDirection', { type: 'checkbox' }))
      ];
      var adv = [];
      if (GMB.isAdvanced()) {
        adv = [
          GMB.field('Steps / revolution', GMB.input(s, 'stepsPerRevolution', { type: 'number', onChange: function () { drawStep(); } })),
          GMB.field('Microsteps', GMB.input(s, 'microsteps', { type: 'number', onChange: function () { drawStep(); } })),
          s.transmission === 'screw'
            ? GMB.field('Lead / rev (mm)', GMB.input(s, 'leadPerRevolutionMm', { type: 'number', onChange: function () { drawStep(); } }))
            : (s.transmission === 'custom'
              ? GMB.field('Custom steps/mm', GMB.input(s, 'customStepsPerMm', { type: 'number', onChange: function () { drawStep(); } }))
              : [GMB.field('Pulley teeth', GMB.input(s, 'pulleyTeeth', { type: 'number', onChange: function () { drawStep(); } })),
                 GMB.field('Belt pitch (mm)', GMB.input(s, 'beltPitchMm', { type: 'number', onChange: function () { drawStep(); } }))]),
          GMB.field('Max speed (mm/s)', GMB.input(s, 'maxSpeedMmS', { type: 'number' })),
          GMB.field('Max accel (mm/s²)', GMB.input(s, 'maxAccelMmS2', { type: 'number' })),
          GMB.field('Min position (mm)', GMB.input(s, 'minPositionMm', { type: 'number' })),
          GMB.field('Max position (mm)', GMB.input(s, 'maxPositionMm', { type: 'number' }))
        ];
      }
      body.appendChild(h('div.substring', [
        h('div.substring-head', [h('strong', 'String ' + (i + 1)), h('span.pill.mini', GMB.noteName(s.openNote)),
          h('span.muted', 'steps/mm = ' + spm.toFixed(2))]),
        h('div.form-grid', basic.concat(adv))
      ]));
    });
  }

  // Assisted steps/mm (cahier des charges 12.1) — mirrors StepperAxis::stepsPerMm.
  function stepsPerMm(s) {
    var full = s.stepsPerRevolution * s.microsteps;
    if (s.transmission === 'screw') return full / (s.leadPerRevolutionMm || 1);
    if (s.transmission === 'custom') return s.customStepsPerMm;
    return full / ((s.pulleyTeeth || 1) * (s.beltPitchMm || 1));
  }
  GMB.stepsPerMm = stepsPerMm;

  // ---- Step 5: Homing -------------------------------------------------------
  function stepHoming(body) {
    body.appendChild(h('h3', 'Homing per axis'));
    GMB.state.profile.strings.forEach(function (s, i) {
      var hm = s.homing;
      var fields = [
        GMB.field('Sensor active level', GMB.input(hm, 'sensorActiveHigh', {
          type: 'select', options: [{ value: true, label: 'Active high' }, { value: false, label: 'Active low' }],
          coerce: function (v) { return v === 'true' || v === true; }
        })),
        GMB.field('Direction', GMB.input(hm, 'direction', {
          type: 'select', options: [{ value: -1, label: 'Toward − (home)' }, { value: 1, label: 'Toward +' }],
          coerce: Number
        }))
      ];
      if (GMB.isAdvanced()) {
        fields = fields.concat([
          GMB.field('Fast speed (mm/s)', GMB.input(hm, 'fastSpeedMmS', { type: 'number' })),
          GMB.field('Slow speed (mm/s)', GMB.input(hm, 'slowSpeedMmS', { type: 'number' })),
          GMB.field('Backoff (mm)', GMB.input(hm, 'backoffMm', { type: 'number' })),
          GMB.field('Offset after zero (mm)', GMB.input(hm, 'offsetMm', { type: 'number' })),
          GMB.field('Timeout (ms)', GMB.input(hm, 'timeoutMs', { type: 'number' })),
          GMB.field('Max search (mm)', GMB.input(hm, 'maxSearchMm', { type: 'number' }))
        ]);
      }
      body.appendChild(h('div.substring', [
        h('div.substring-head', [h('strong', 'String ' + (i + 1) + ' homing')]),
        h('div.form-grid', fields)
      ]));
    });
    body.appendChild(h('div.toolbar', [GMB.button('Start homing (test)', function () {
      GMB.api.testNote({ homing: true, string: 0, fret: 0, stringCc: 20, fretCc: 21, note: 60, velocity: 1, channel: 0 })
        .then(function () { GMB.toast('Homing command sent to all axes.', 'ok'); });
    }, 'primary')]));
  }

  // ---- Step 6: Servos -------------------------------------------------------
  function stepServos(body) {
    body.appendChild(h('h3', 'Servo calibration'));
    var p = GMB.state.profile;
    p.servos.forEach(function (sv, i) {
      var fields = [
        GMB.field('Enabled', GMB.input(sv, 'enabled', { type: 'checkbox' })),
        GMB.field('Function', GMB.input(sv, 'function', {
          type: 'select', options: ['finger', 'pluck', 'damper', 'aux']
        })),
        GMB.field('PCA9685 channel', GMB.input(sv, 'channel', { type: 'number', min: 0, max: 15 })),
        GMB.field('Rest (µs)', GMB.input(sv, 'restUs', { type: 'number' })),
        GMB.field('Active (µs)', GMB.input(sv, 'activeUs', { type: 'number' }))
      ];
      if (GMB.isAdvanced()) {
        fields = fields.concat([
          GMB.field('Pulse min (µs)', GMB.input(sv, 'pulseMinUs', { type: 'number' })),
          GMB.field('Pulse max (µs)', GMB.input(sv, 'pulseMaxUs', { type: 'number' })),
          GMB.field('Inverted', GMB.input(sv, 'inverted', { type: 'checkbox' })),
          GMB.field('Travel (ms)', GMB.input(sv, 'travelMs', { type: 'number' })),
          GMB.field('Settle (ms)', GMB.input(sv, 'settleMs', { type: 'number' })),
          GMB.field('Disable at rest', GMB.input(sv, 'disableAtRest', { type: 'checkbox' }))
        ]);
      }
      body.appendChild(h('div.substring', [
        h('div.substring-head', [h('strong', 'Servo ' + (i + 1) + ' — ' + sv.function),
          h('span.muted', 'channel ' + sv.channel)]),
        h('div.form-grid', fields)
      ]));
    });
    body.appendChild(h('div.toolbar', [
      GMB.button('Add servo', function () {
        p.servos.push({ enabled: true, channel: p.servos.length, function: 'finger', pulseMinUs: 500,
          pulseMaxUs: 2500, restUs: 1000, activeUs: 1800, inverted: false, travelMs: 120, settleMs: 30, disableAtRest: true });
        GMB.markDirty(); drawStep();
      }, 'ghost')
    ]));
  }

  // ---- Step 7: Notes --------------------------------------------------------
  function stepNotes(body) {
    body.appendChild(h('h3', 'Note calibration'));
    body.appendChild(h('p.muted', 'Fret positions can be computed theoretically or calibrated by hand (the calibrated table wins).'));
    GMB.state.profile.strings.forEach(function (s, i) {
      body.appendChild(h('div.substring', [
        h('div.substring-head', [h('strong', 'String ' + (i + 1)),
          GMB.field('Open note (MIDI)', GMB.input(s, 'openNote', { type: 'number', min: 0, max: 127, onChange: function () { drawStep(); } })),
          h('span.pill.mini', GMB.noteName(s.openNote))]),
        fretTable(s)
      ]));
    });
  }

  function fretTable(s) {
    var rows = [];
    for (var f = 0; f <= s.maxFret; f++) {
      var theo = s.scaleLengthMm * (1 - Math.pow(2, -f / 12)); // cahier des charges 14.2
      var cal = s.calibratedFretMm[f];
      rows.push(h('tr', [
        h('td', String(f)),
        h('td', GMB.noteName(s.openNote + f)),
        h('td', theo.toFixed(2)),
        h('td', cal === undefined || cal === null ? h('span.muted', '—') : cal.toFixed(2))
      ]));
    }
    return h('div.table-wrap', h('table.mini-table', [
      h('thead', h('tr', [h('th', 'Fret'), h('th', 'Note'), h('th', 'Theoretical mm'), h('th', 'Calibrated mm')])),
      h('tbody', rows)
    ]));
  }

  // ---- Step 8: Test ---------------------------------------------------------
  function stepTest(body) {
    body.appendChild(h('h3', 'Test'));
    body.appendChild(h('p', 'Fire individual actuators and notes. In normal mode nothing actuates until critical errors are cleared.'));
    var p = GMB.state.profile;
    var testWrap = h('div.toolbar.wrap');
    p.strings.forEach(function (s, i) {
      testWrap.appendChild(GMB.button('Test string ' + (i + 1), function () {
        GMB.api.testNote({ string: i + 1, fret: 0, stringCc: p.stringFretSelection.string.ccNumber,
          fretCc: p.stringFretSelection.fret.ccNumber, note: s.openNote, velocity: 100, channel: 0 })
          .then(function () { GMB.toast('Tested string ' + (i + 1), 'ok'); });
      }, 'ghost'));
    });
    testWrap.appendChild(GMB.button('Test chord', function () { GMB.toast('Chord test sent.', 'ok'); }, 'ghost'));
    testWrap.appendChild(GMB.button('STOP', GMB.doPanic, 'danger'));
    body.appendChild(testWrap);
    body.appendChild(h('p.muted', 'Full note/string/fret testing with a step trace lives on the MIDI page.'));
    body.appendChild(GMB.button('Open MIDI test tool', function () { GMB.navigate('midi'); }, 'primary'));
  }

  // ---- Step 9: Validation ---------------------------------------------------
  function stepValidation(body) {
    body.appendChild(h('h3', 'Validation'));
    var problems = GMB.validateProfile(GMB.state.profile);
    if (!problems.length) {
      body.appendChild(h('div.big-ok', 'Configuration valid'));
      body.appendChild(h('p.muted', 'You can now save & publish. Capabilities will be rebuilt and announced by SysEx.'));
    } else {
      body.appendChild(h('div.big-warn', problems.length + ' issue(s) to fix'));
      body.appendChild(h('ul.problem-list', problems.map(function (p) { return h('li', p); })));
      body.appendChild(h('p.muted', 'No actuators run in normal mode until critical errors are corrected.'));
    }
  }

  // Cross-field profile validation, shared with the SysEx / profiles pages.
  GMB.validateProfile = function (p) {
    var out = [];
    if (!p.instrument.name) out.push('Instrument name is empty.');
    if (p.instrument.stringCount < 1 || p.instrument.stringCount > 6) out.push('String count must be 1–6.');
    if (p.strings.length !== p.instrument.stringCount) out.push('String array size does not match the string count.');
    // Pin conflicts (basic duplicate check; full check on the Pins page).
    var seen = {};
    p.pins.forEach(function (a) {
      if (a.gpio < 0) { out.push(a.signal + ' has no GPIO assigned.'); return; }
      if (seen[a.gpio]) out.push('GPIO' + a.gpio + ' used by both ' + seen[a.gpio] + ' and ' + a.signal + '.');
      else seen[a.gpio] = a.signal;
    });
    var sfs = p.stringFretSelection;
    if (sfs.string.ccNumber === sfs.fret.ccNumber) out.push('String CC and fret CC must differ.');
    if (sfs.string.ccNumber > 119 || sfs.fret.ccNumber > 119) out.push('CC numbers must be 0–119 (120–127 are channel-mode messages).');
    if (sfs.selectionTimeoutMs < 5 || sfs.selectionTimeoutMs > 2000) out.push('Selection timeout must be 5–2000 ms.');
    if (sfs.queueDepth < 16) out.push('Selection queue depth must be at least 16.');
    return out;
  };

  GMB.views.wizard = { render: render, reset: function () { step = 0; } };
})(window);
