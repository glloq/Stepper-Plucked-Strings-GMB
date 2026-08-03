/*
 * wizard.js — first-configuration assistant (spec section 10).
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
  var board = null;              // board profile (for GPIO capability filtering)
  var motorPos = {};            // per-string live motor position (fret editor)
  var activeStr = 0;            // per-string steps show ONE string at a time
  var statusConn = null;        // live /ws/status subscription (motor readback)

  // Tab strip to pick which string the per-string steps (Mechanics, Homing,
  // Servos, Notes) show — one string at a time keeps long instruments navigable
  // and stops full-step re-renders from throwing away the scroll position.
  function stringTabs() {
    var n = GMB.state.profile.instrument.stringCount;
    if (activeStr >= n) activeStr = n - 1;
    if (activeStr < 0) activeStr = 0;
    var tabs = [];
    for (var i = 0; i < n; i++) {
      (function (idx) {
        tabs.push(h('button.strtab' + (idx === activeStr ? '.active' : ''),
          { onclick: function () { activeStr = idx; drawStep(); } },
          'String ' + (idx + 1)));
      })(i);
    }
    return h('div.strtabs', tabs);
  }

  // Copy one string's settings to every other string (uniform mechanics are the
  // common case; per-string wiring like servos/pins is intentionally NOT copied).
  function copyMechToAll(src) {
    var keys = ['scaleLengthMm', 'transmission', 'stepsPerRevolution', 'microsteps',
      'pulleyTeeth', 'beltPitchMm', 'leadPerRevolutionMm', 'customStepsPerMm',
      'invertDirection', 'minPositionMm', 'maxPositionMm', 'maxSpeedMmS', 'maxAccelMmS2', 'enabled'];
    GMB.state.profile.strings.forEach(function (st) {
      if (st === src) return;
      keys.forEach(function (k) { if (src[k] !== undefined) st[k] = src[k]; });
    });
    GMB.markDirty(); drawStep(); GMB.toast('Mechanics copied to all strings.', 'ok');
  }
  function copyHomingToAll(src) {
    GMB.state.profile.strings.forEach(function (st) {
      if (st !== src) st.homing = GMB.deepCopy(src.homing);
    });
    GMB.markDirty(); drawStep(); GMB.toast('Homing copied to all strings.', 'ok');
  }
  function copyFretsToAll(src) {
    GMB.state.profile.strings.forEach(function (st) {
      if (st === src) return;
      st.scaleLengthMm = src.scaleLengthMm;
      st.calibratedFretMm = (src.calibratedFretMm || []).slice();
    });
    GMB.markDirty(); drawStep(); GMB.toast('Scale + fret calibration copied to all strings.', 'ok');
  }
  function copyToAllBtn(label, fn, src) {
    return GMB.button(label, function () { fn(src); }, 'ghost');
  }

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
    // Subscribe once to live status so the Notes step can read the real motor
    // position for fret capture. Closed in reset().
    if (!statusConn && GMB.api && GMB.api.connectStatus) {
      statusConn = GMB.api.connectStatus(function (st) { onWizardStatus(st); });
    }
    // The Homing/Servos steps filter free GPIOs, so make sure the board profile
    // is loaded (mirrors pins.js). Cached after the first fetch.
    if (!board) {
      GMB.api.getBoard(GMB.state.profile.board.profile).then(function (b) { board = b; GMB.render(); });
      host.appendChild(h('div.card', 'Loading board profile…'));
      return;
    }
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
      GMB.field('Max frets (all strings)', GMB.input(strings0(), 'maxFret', {
        type: 'number', min: 0, max: 30,
        onChange: function (v) {
          var mf = Number(v);
          GMB.state.profile.strings.forEach(function (st) { st.maxFret = mf; });
          GMB.markDirty();
        }
      }), 'Applied to every string; fine-tune per string in the Notes step.'),
      GMB.field('Plucking mode', GMB.input(inst, 'pluckMode', {
        type: 'select', options: [
          { value: 'individual', label: 'Individual pluck (one plucker per string)' },
          { value: 'sharedStrum', label: 'Shared strummer' },
          { value: 'both', label: 'Both' }]
      }), 'How strings are excited; drives the servo roles you need.'),
      GMB.field('Capo (fret)', GMB.input(inst, 'capo', { type: 'number', min: 0, max: 12 }),
        'Global capo: raises every open string by this many frets.'),
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
      enabled: true,
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

    // ---- Network / Wi-Fi -----------------------------------------------------
    var net = p.network;
    var station = net.mode === 'station';
    var netFields = [
      GMB.field('Wi-Fi mode', GMB.input(net, 'mode', {
        type: 'select', options: [
          { value: 'accessPoint', label: 'Access point (hosts its own network)' },
          { value: 'station', label: 'Station (joins your Wi-Fi)' }],
        onChange: function () { drawStep(); }
      })),
      GMB.field('Hostname (mDNS)', GMB.input(net, 'hostname'))
    ];
    if (station) {
      netFields.push(GMB.field('Network SSID', GMB.input(net, 'ssid'), 'The Wi-Fi network to join.'));
      netFields.push(GMB.field('Static IP', GMB.input(net, 'staticIp', { type: 'checkbox' })));
    } else {
      netFields.push(GMB.field('Access-point name', GMB.input(net, 'apSsid'), 'SSID the instrument broadcasts.'));
    }
    body.appendChild(h('div.card', [
      h('h3', 'Network'),
      h('p.muted', 'The Wi-Fi password is set separately (never stored in the exported profile).'),
      h('div.form-grid', netFields)
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
    body.appendChild(stringTabs());
    var i = activeStr, s = GMB.state.profile.strings[i];
    if (!s) return;
    var spm = stepsPerMm(s);
    if (s.enabled === undefined) s.enabled = true;
    // Motion (speed/accel/direction) is now in the simplified view: the user
    // wants it easy to set per string. Only fine geometry stays Advanced.
    var basic = [
      GMB.field('Axis enabled', GMB.input(s, 'enabled', { type: 'checkbox', onChange: function () { drawStep(); } }),
        'Uncheck to skip a broken axis without losing its tuning/pins.'),
      GMB.field('Scale length (mm)', GMB.input(s, 'scaleLengthMm', { type: 'number', onChange: function () { drawStep(); } })),
      GMB.field('Transmission', GMB.input(s, 'transmission', {
        type: 'select', options: [{ value: 'beltGt2', label: 'GT2 belt' }, { value: 'screw', label: 'Screw' }, { value: 'custom', label: 'Custom' }],
        onChange: function () { drawStep(); }
      })),
      GMB.field('Motor wiring polarity (invert direction)', GMB.input(s, 'invertDirection', { type: 'checkbox' }),
        'Flip if the carriage moves away from HOME during homing.'),
      GMB.field('Max speed (mm/s)', GMB.input(s, 'maxSpeedMmS', { type: 'number' })),
      GMB.field('Max acceleration (mm/s²)', GMB.input(s, 'maxAccelMmS2', { type: 'number' }),
        'Motion is trapezoidal (FastAccelStepper) — this is the accel magnitude.')
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
        GMB.field('Min position (mm)', GMB.input(s, 'minPositionMm', { type: 'number', onChange: function () { drawStep(); } })),
        GMB.field('Max position (mm)', GMB.input(s, 'maxPositionMm', { type: 'number' }))
      ];
    }
    body.appendChild(h('div.substring', [
      h('div.substring-head', [h('strong', 'String ' + (i + 1)), h('span.pill.mini', GMB.noteName(s.openNote)),
        h('span.muted', 'steps/mm = ' + spm.toFixed(2))]),
      h('div.form-grid', basic.concat(adv)),
      h('div.toolbar', [copyToAllBtn('Copy mechanics to all strings', copyMechToAll, s)])
    ]));
  }

  // Assisted steps/mm (spec 12.1) — mirrors StepperAxis::stepsPerMm.
  function stepsPerMm(s) {
    var full = s.stepsPerRevolution * s.microsteps;
    if (s.transmission === 'screw') return full / (s.leadPerRevolutionMm || 1);
    if (s.transmission === 'custom') return s.customStepsPerMm;
    return full / ((s.pulleyTeeth || 1) * (s.beltPitchMm || 1));
  }
  GMB.stepsPerMm = stepsPerMm;

  // ---- Shared GPIO / servo helpers (Homing + Servos steps) ------------------
  // Reuses the same capability filtering as pins.js: reserved/USB pins are never
  // offered, caution pins only in Advanced, and anything already used by a
  // stepper signal or another servo is excluded.
  function pinSignalGpio(signal) {
    var a = GMB.state.profile.pins.filter(function (x) { return x.signal === signal; })[0];
    return a ? a.gpio : -1;
  }
  function setPinSignal(signal, kind, gpio) {
    var pins = GMB.state.profile.pins;
    var a = pins.filter(function (x) { return x.signal === signal; })[0];
    if (gpio < 0 && kind === 'limit') {                 // optional signal: drop when cleared
      if (a) pins.splice(pins.indexOf(a), 1);
      GMB.markDirty();
      return;
    }
    if (!a) { a = { signal: signal, kind: kind, gpio: -1 }; pins.push(a); }
    a.gpio = gpio; a.kind = kind;
    GMB.markDirty();
  }
  // Map of GPIO -> owner label, excluding one signal or one servo being edited.
  function usedGpios(opts) {
    opts = opts || {};
    var m = {};
    GMB.state.profile.pins.forEach(function (a) {
      if (a.gpio >= 0 && a.signal !== opts.exceptSignal) m[a.gpio] = 'signal ' + a.signal;
    });
    (GMB.state.profile.servos || []).forEach(function (sv) {
      if (sv === opts.exceptServo) return;
      if (sv.source === 'gpio' && sv.gpio >= 0) m[sv.gpio] = 'servo ' + (sv.function || '');
    });
    return m;
  }
  function gpioCandidates(kind, used) {
    var reserveUsb = GMB.state.profile.board.reserveUsb;
    var wantKind = GMB.SIGNAL_KIND[kind] || 'generic';
    return board.pins.filter(function (cap) {
      if (used[cap.gpio]) return false;
      if (cap.reserved || cap.preference === 'reserved') return false;
      if (cap.usb && reserveUsb) return false;
      if (cap.preference === 'caution' && !GMB.isAdvanced()) return false;
      return GMB.pinSupports(cap, wantKind);
    });
  }
  // A <select> of compatible free GPIOs; keeps the current pin visible even if
  // it is a caution pin so an advanced choice survives a mode switch.
  function gpioSelect(kind, curGpio, used, onChange) {
    var sel = h('select');
    sel.appendChild(h('option', { value: -1, selected: curGpio < 0 }, '— unassigned —'));
    var seen = {};
    gpioCandidates(kind, used).forEach(function (cap) {
      seen[cap.gpio] = true;
      sel.appendChild(h('option', { value: cap.gpio, selected: cap.gpio === curGpio },
        'GPIO' + cap.gpio + (cap.preference === 'caution' ? ' (caution)' : '')));
    });
    if (curGpio >= 0 && !seen[curGpio]) {
      sel.appendChild(h('option', { value: curGpio, selected: true }, 'GPIO' + curGpio + ' (current)'));
    }
    sel.addEventListener('change', function () { onChange(Number(sel.value)); });
    return sel;
  }

  // ---- Step 5: Homing & endstops -------------------------------------------
  function stepHoming(body) {
    body.appendChild(h('h3', 'Homing & endstops per string'));
    body.appendChild(h('p.muted', 'Each string homes on its own HOME switch. Pick the GPIO and the homing behaviour; a LIMIT switch is optional (Advanced).'));
    body.appendChild(stringTabs());
    var i = activeStr, s = GMB.state.profile.strings[i];
    if (!s) return;
    var hm = s.homing;
    var homeSignal = 'HOME' + (i + 1), limitSignal = 'LIMIT' + (i + 1);
    var homeGpio = pinSignalGpio(homeSignal), limitGpio = pinSignalGpio(limitSignal);
    var fields = [
      GMB.field('HOME sensor GPIO',
        gpioSelect('home', homeGpio, usedGpios({ exceptSignal: homeSignal }),
          function (g) { setPinSignal(homeSignal, 'home', g); drawStep(); }),
        'Endstop that defines the zero (FDC).'),
      GMB.field('Sensor active level', GMB.input(hm, 'sensorActiveHigh', {
        type: 'select', options: [{ value: true, label: 'Active high' }, { value: false, label: 'Active low' }],
        coerce: function (v) { return v === 'true' || v === true; }
      })),
      GMB.field('Homing search direction', GMB.input(hm, 'direction', {
        type: 'select', options: [{ value: -1, label: 'Toward − (home)' }, { value: 1, label: 'Toward +' }],
        coerce: Number
      }), 'Which way the carriage moves to find HOME.'),
      GMB.field('Zero offset / rest position (mm)', GMB.input(hm, 'offsetMm', { type: 'number' }),
        'Where the axis rests past the HOME sensor (the FDC position).')
    ];
    if (GMB.isAdvanced()) {
      fields = fields.concat([
        GMB.field('Fast speed (mm/s)', GMB.input(hm, 'fastSpeedMmS', { type: 'number' })),
        GMB.field('Slow speed (mm/s)', GMB.input(hm, 'slowSpeedMmS', { type: 'number' })),
        GMB.field('Backoff (mm)', GMB.input(hm, 'backoffMm', { type: 'number' })),
        GMB.field('Timeout (ms)', GMB.input(hm, 'timeoutMs', { type: 'number' })),
        GMB.field('Max search (mm)', GMB.input(hm, 'maxSearchMm', { type: 'number' })),
        GMB.field('LIMIT switch GPIO (optional)',
          gpioSelect('limit', limitGpio, usedGpios({ exceptSignal: limitSignal }),
            function (g) { setPinSignal(limitSignal, 'limit', g); drawStep(); }),
          'Optional end-of-travel safety switch.'),
        GMB.field('LIMIT active level', GMB.input(hm, 'limitActiveHigh', {
          type: 'select', options: [{ value: false, label: 'Active low' }, { value: true, label: 'Active high' }],
          coerce: function (v) { return v === 'true' || v === true; }
        }))
      ]);
    }
    var readout = h('div.endstop-readout', h('span.muted', 'Press “Test endstop” to read the live level.'));
    body.appendChild(h('div.substring', [
      h('div.substring-head', [h('strong', 'String ' + (i + 1) + ' homing'),
        h('span.pill.mini', GMB.noteName(s.openNote)),
        homeGpio < 0 ? h('span.pill.mini.error', 'no HOME pin') : h('span.pill.mini.ok', 'HOME GPIO' + homeGpio)]),
      h('div.form-grid', fields),
      h('div.toolbar.wrap', [
        GMB.button('Test endstop', function () { testEndstop(i, s, readout); }, 'ghost'),
        copyToAllBtn('Copy homing to all strings', copyHomingToAll, s)
      ]),
      readout
    ]));
    body.appendChild(h('div.toolbar', [GMB.button('Home all axes now', function () {
      GMB.api.resetSystem().then(function (res) {
        if (res && res.ok === false) {
          GMB.toast('Homing refused: ' + (res.error || 'E-stop/LIMIT active or invalid config') + '.', 'warn');
        } else {
          GMB.toast('Homing started on all axes.', 'ok');
        }
      }).catch(function (e) { testErr('Homing failed', e); });
    }, 'primary')]));
  }

  // POST /api/test/endstop -> { ok:true, home:Bool, limit:Bool } for axis i.
  function testEndstop(i, s, readout) {
    var homeGpio = pinSignalGpio('HOME' + (i + 1)), limitGpio = pinSignalGpio('LIMIT' + (i + 1));
    GMB.api.testEndstop({ axis: i }).then(function (res) {
      readout.innerHTML = '';
      if (homeGpio < 0) {
        readout.appendChild(h('span.pill.mini.error', 'HOME switch has no GPIO assigned.'));
      } else {
        readout.appendChild(endstopLed('HOME', homeGpio, !!res.home));
      }
      if (limitGpio >= 0) readout.appendChild(endstopLed('LIMIT', limitGpio, !!res.limit));
    }).catch(function (e) { testErr('Endstop read failed', e); });
  }
  function endstopLed(name, gpio, active) {
    return h('div.endstop-line', [
      h('span.leddot' + (active ? '.on' : '')),
      h('span.endstop-name', name + ' GPIO' + gpio),
      h('span.pill.mini' + (active ? '.ok' : '.muted'), active ? 'active' : 'idle')
    ]);
  }

  // ---- Step 6: Servos per string -------------------------------------------
  var ROLE_LABEL = {
    finger: 'Finger', pluck: 'Pluck (plectrum)', strum: 'Strum', strumLift: 'Strum lift',
    damper: 'Damper', sharedStrum: 'Shared strum', sharedDamper: 'Shared damper', aux: 'Auxiliary'
  };
  function roleLabel(fn) { return ROLE_LABEL[fn] || fn; }

  function stepServos(body) {
    body.appendChild(h('h3', 'Servos per string'));
    body.appendChild(h('p.muted', 'Add the servos each string uses. Every servo is driven either by a PCA9685 channel or directly from a free ESP32 GPIO — mix them freely, or use no PCA at all.'));
    body.appendChild(channelMap());
    body.appendChild(stringTabs());
    body.appendChild(stringServoSection(activeStr));
    if (GMB.isAdvanced()) body.appendChild(sharedServoSection());
  }

  // Compact PCA channel availability map; duplicates are flagged in red.
  function channelMap() {
    var seen = {}, dup = {}, boardsUsed = {};
    (GMB.state.profile.servos || []).forEach(function (sv) {
      if (!sv.enabled || sv.source !== 'pca') return;
      var key = sv.pcaBoard + ':' + sv.channel;
      boardsUsed[sv.pcaBoard] = true;
      if (seen[key]) dup[key] = true; else seen[key] = sv;
    });
    var boards = Object.keys(boardsUsed).map(Number).sort(function (a, b) { return a - b; });
    if (!boards.length) {
      return h('div.note-box', [h('strong', 'No PCA9685 channel in use'),
        ' — every servo is on a direct GPIO (or none added yet).']);
    }
    return h('div.chan-map', boards.map(function (b) {
      var chips = [];
      for (var c = 0; c < 16; c++) {
        var key = b + ':' + c;
        var cls = dup[key] ? 'dup' : (seen[key] ? 'used' : 'free');
        chips.push(h('span.chan-chip.' + cls, { title: seen[key] ? roleLabel(seen[key].function) : 'free' }, String(c)));
      }
      return h('div.chan-row', [
        h('span.chan-board', 'Board ' + b + ' (0x' + (0x40 + b).toString(16) + ')'),
        h('div.chan-chips', chips)
      ]);
    }));
  }

  function stringServoSection(i) {
    var p = GMB.state.profile, s = p.strings[i];
    var servos = p.servos.filter(function (sv) { return sv.stringIndex === i; });
    return h('div.substring', [
      h('div.substring-head', [h('strong', 'String ' + (i + 1)), h('span.pill.mini', GMB.noteName(s.openNote)),
        h('span.muted', servos.length + ' servo' + (servos.length === 1 ? '' : 's'))]),
      servos.length ? h('div.servo-list', servos.map(servoRow)) : h('p.muted', 'No servo yet — add one below.'),
      h('div.toolbar.wrap', [
        GMB.button('+ Finger', function () { addServo('finger', i); }, 'ghost'),
        GMB.button('+ Strum', function () { addServo('strum', i); }, 'ghost'),
        GMB.button('+ Strum lift', function () { addServo('strumLift', i); }, 'ghost'),
        GMB.button('+ Damper', function () { addServo('damper', i); }, 'ghost'),
        GMB.button('+ Pluck (plectrum)', function () { addServo('pluck', i); }, 'ghost')
      ])
    ]);
  }

  function sharedServoSection() {
    var p = GMB.state.profile;
    var servos = p.servos.filter(function (sv) { return sv.stringIndex === -1; });
    return h('div.substring', [
      h('div.substring-head', [h('strong', 'Shared / auxiliary servos'), h('span.muted', 'stringIndex −1 (span several strings)')]),
      servos.length ? h('div.servo-list', servos.map(servoRow)) : h('p.muted', 'No shared servo.'),
      h('div.toolbar.wrap', [
        GMB.button('+ Shared strum', function () { addServo('sharedStrum', -1); }, 'ghost'),
        GMB.button('+ Auxiliary', function () { addServo('aux', -1); }, 'ghost')
      ])
    ]);
  }

  function nextFreeChannel(bd) {
    var used = {};
    GMB.state.profile.servos.forEach(function (sv) {
      if (sv.source === 'pca' && sv.pcaBoard === bd) used[sv.channel] = true;
    });
    for (var c = 0; c < 16; c++) if (!used[c]) return c;
    return 0;
  }
  function addServo(fn, stringIndex) {
    var opts = { source: 'pca', pcaBoard: 0, channel: nextFreeChannel(0) };
    if (fn === 'pluck' || fn === 'strum' || fn === 'sharedStrum') { opts.activeUs = 1700; opts.travelMs = 90; opts.settleMs = 20; }
    if (fn === 'damper' || fn === 'sharedDamper') { opts.activeUs = 1600; }
    GMB.state.profile.servos.push(GMB.servoDefaults(fn, stringIndex, opts));
    GMB.markDirty(); drawStep();
  }
  function removeServo(sv) {
    var arr = GMB.state.profile.servos, idx = arr.indexOf(sv);
    if (idx >= 0) arr.splice(idx, 1);
    GMB.markDirty(); drawStep();
  }
  function onSourceChange(sv, v) {
    if (v === 'gpio') { if (sv.gpio === undefined) sv.gpio = -1; }
    else { sv.gpio = -1; sv.channel = nextFreeChannel(sv.pcaBoard || 0); }
    GMB.markDirty(); drawStep();
  }
  function pcaDuplicate(sv) {
    if (sv.source !== 'pca' || !sv.enabled) return false;
    return GMB.state.profile.servos.some(function (o) {
      return o !== sv && o.source === 'pca' && o.enabled && o.pcaBoard === sv.pcaBoard && o.channel === sv.channel;
    });
  }
  function testServo(sv, to) {
    var index = GMB.state.profile.servos.indexOf(sv);
    GMB.api.testServo({
      index: index, active: to === 'active',
      function: sv.function, source: sv.source, pcaBoard: sv.pcaBoard, channel: sv.channel,
      gpio: sv.gpio, restUs: sv.restUs, activeUs: sv.activeUs
    }).then(function (res) {
      if (res && res.ok === false) { GMB.toast('Servo not driven: ' + (res.error || 'actuators not armed') + '.', 'warn'); return; }
      GMB.toast(res.message || ('Servo driven to ' + to + '.'), 'ok');
    }).catch(function (e) { testErr('Servo test failed', e); });
  }

  // Surface a 409/other backend error from a /api/test/* call.
  function testErr(prefix, e) {
    var body = e && e.body;
    GMB.toast(prefix + ': ' + ((body && body.error) || (e && e.message) || 'error'), 'error');
  }

  function servoRow(sv) {
    var srcFields;
    if (sv.source === 'gpio') {
      srcFields = GMB.field('Direct GPIO',
        gpioSelect('servo', sv.gpio, usedGpios({ exceptServo: sv }),
          function (g) { sv.gpio = g; GMB.markDirty(); drawStep(); }),
        'Free ESP32 pin (50 Hz LEDC PWM).');
    } else {
      srcFields = [
        GMB.field('PCA board', GMB.input(sv, 'pcaBoard', {
          type: 'select', coerce: Number, onChange: function () { onSourceChange(sv, 'pca'); },
          options: [{ value: 0, label: 'Board 0 (0x40)' }, { value: 1, label: 'Board 1 (0x41)' },
            { value: 2, label: 'Board 2 (0x42)' }, { value: 3, label: 'Board 3 (0x43)' }]
        })),
        GMB.field('Channel (0–15)', GMB.input(sv, 'channel', {
          type: 'number', min: 0, max: 15, onChange: function () { drawStep(); }
        }), pcaDuplicate(sv) ? '⚠ board + channel already used' : 'PCA9685 output channel')
      ];
    }
    var dup = pcaDuplicate(sv);
    var head = h('div.servo-head', [
      h('strong', roleLabel(sv.function)),
      h('span.pill.mini', sv.source === 'gpio'
        ? ('GPIO' + (sv.gpio >= 0 ? sv.gpio : '—'))
        : ('B' + sv.pcaBoard + ' · ch' + sv.channel)),
      sv.enabled ? null : h('span.pill.mini.muted', 'disabled'),
      dup ? h('span.pill.mini.error', 'duplicate') : null
    ]);
    var fields = [
      GMB.field('Enabled', GMB.input(sv, 'enabled', { type: 'checkbox', onChange: function () { drawStep(); } })),
      GMB.field('Signal source', GMB.input(sv, 'source', {
        type: 'select', options: [{ value: 'pca', label: 'PCA9685' }, { value: 'gpio', label: 'Direct GPIO' }],
        onChange: function (v) { onSourceChange(sv, v); }
      }))
    ];
    fields = fields.concat(Array.isArray(srcFields) ? srcFields : [srcFields]);
    fields = fields.concat([
      GMB.field('Rest (µs)', GMB.input(sv, 'restUs', { type: 'number' })),
      GMB.field('Active (µs)', GMB.input(sv, 'activeUs', { type: 'number' }))
    ]);
    // Strum / stroke motion — shown for the roles that actually strike a string.
    var role = sv.function;
    var isStriker = role === 'strum' || role === 'pluck' || role === 'sharedStrum';
    if (role === 'strumLift') {
      fields.push(GMB.field('Engage delay (ms)', GMB.input(sv, 'engageDelayMs', { type: 'number', min: 0 }),
        'Extra pause after the lift is down, before the strum stroke fires.'));
    }
    if (isStriker) {
      fields.push(GMB.field('Alternate stroke direction', GMB.input(sv, 'alternateDirection', {
        type: 'checkbox', onChange: function () { drawStep(); } }),
        'Down-stroke, up-stroke, down-stroke… on successive strokes.'));
      if (sv.alternateDirection) {
        fields.push(GMB.field('Up-stroke active (µs, 0 = mirror rest)', GMB.input(sv, 'activeAltUs', { type: 'number', min: 0 })));
      }
      fields.push(GMB.field('Stroke time (ms, 0 = use travel)', GMB.input(sv, 'strokeMs', { type: 'number', min: 0 }),
        'How long the stroke stays engaged (its speed), independent of the return.'));
      fields.push(GMB.field('Min strike depth (µs, 0 = off)', GMB.input(sv, 'minStrikeUs', { type: 'number', min: 0 }),
        'Guaranteed depth toward the string so soft notes still catch it.'));
    }
    if (GMB.isAdvanced()) {
      fields = fields.concat([
        GMB.field('Function', GMB.input(sv, 'function', {
          type: 'select', options: ['finger', 'pluck', 'strum', 'strumLift', 'damper', 'sharedStrum', 'sharedDamper', 'aux'],
          onChange: function () { drawStep(); }
        })),
        GMB.field('Pulse min (µs)', GMB.input(sv, 'pulseMinUs', { type: 'number' })),
        GMB.field('Pulse max (µs)', GMB.input(sv, 'pulseMaxUs', { type: 'number' })),
        GMB.field('Inverted', GMB.input(sv, 'inverted', { type: 'checkbox' })),
        GMB.field('Travel (ms)', GMB.input(sv, 'travelMs', { type: 'number' })),
        GMB.field('Settle (ms)', GMB.input(sv, 'settleMs', { type: 'number' })),
        GMB.field('Disable at rest', GMB.input(sv, 'disableAtRest', { type: 'checkbox' }))
      ]);
    }
    return h('div.servo-row', [
      head,
      h('div.form-grid', fields),
      h('div.toolbar', [
        GMB.button('Test rest', function () { testServo(sv, 'rest'); }, 'ghost'),
        GMB.button('Test active', function () { testServo(sv, 'active'); }, 'ghost'),
        isStriker ? GMB.button('Test strike', function () {
          testServo(sv, 'active');
          setTimeout(function () { testServo(sv, 'rest'); }, 250);
        }, 'ghost') : null,
        h('span.spacer'),
        GMB.button('Remove', function () { removeServo(sv); }, 'danger-ghost')
      ])
    ]);
  }

  // ---- Step 7: Notes / fret position editor --------------------------------
  function stepNotes(body) {
    body.appendChild(h('h3', 'Fret positions per string'));
    body.appendChild(h('p.muted', 'Auto-fill the theoretical positions, then fine-tune any fret by hand. Move the axis to the real fret and use “Capture position” to record the live motor position. A calibrated value always overrides theory in the firmware.'));
    body.appendChild(stringTabs());
    var i = activeStr, s = GMB.state.profile.strings[i];
    if (!s) return;
    var mp = motorPos[i] !== undefined ? motorPos[i] : 0;
    body.appendChild(h('div.substring', [
      h('div.substring-head', [h('strong', 'String ' + (i + 1)),
        GMB.field('Open note (MIDI)', GMB.input(s, 'openNote', { type: 'number', min: 0, max: 127, onChange: function () { drawStep(); } })),
        GMB.field('Max frets', GMB.input(s, 'maxFret', { type: 'number', min: 0, max: 30, onChange: function () { drawStep(); } })),
        h('span.pill.mini', GMB.noteName(s.openNote)),
        h('span.motor-pos', { id: 'motor-pos-live' }, 'Motor: ' + mp.toFixed(2) + ' mm')]),
      h('div.toolbar.wrap', [
        GMB.button('Auto-fill (theoretical)', function () { autoFill(s); }, 'primary'),
        GMB.button('Clear calibration', function () { s.calibratedFretMm = []; GMB.markDirty(); drawStep(); }, 'ghost'),
        copyToAllBtn('Copy scale + calibration to all', copyFretsToAll, s)
      ]),
      fretEditor(s, i)
    ]));
  }

  // Live per-axis motor position from /ws/status, so “Capture position” records
  // the real position instead of a mock (audit: manual calibration was circular).
  function onWizardStatus(st) {
    if (!st || !st.strings) return;
    st.strings.forEach(function (row) {
      if (row && row.index !== undefined && row.positionMm !== undefined) motorPos[row.index] = row.positionMm;
    });
    var live = document.getElementById('motor-pos-live');
    if (live && motorPos[activeStr] !== undefined) live.textContent = 'Motor: ' + Number(motorPos[activeStr]).toFixed(2) + ' mm';
  }

  function autoFill(s) {
    s.calibratedFretMm = [];
    for (var f = 0; f <= s.maxFret; f++) s.calibratedFretMm[f] = +GMB.fretTheoreticalMm(s, f).toFixed(2);
    GMB.markDirty(); drawStep();
  }

  function fretEditor(s, i) {
    var rows = [];
    for (var f = 0; f <= s.maxFret; f++) rows.push(fretRow(s, i, f));
    return h('div.table-wrap', h('table.mini-table.fret-editor', [
      h('thead', h('tr', [h('th', 'Fret'), h('th', 'Note'), h('th', 'Theory mm'), h('th', 'Calibrated mm'), h('th', 'Move / save')])),
      h('tbody', rows)
    ]));
  }

  function fretRow(s, i, f) {
    var theo = GMB.fretTheoreticalMm(s, f);
    var cal = s.calibratedFretMm[f];
    var input = h('input', { type: 'number', step: '0.1', placeholder: theo.toFixed(2),
      value: (cal === undefined || cal === null) ? '' : cal });
    input.addEventListener('change', function () {
      s.calibratedFretMm[f] = input.value === '' ? null : Number(input.value);
      GMB.markDirty(); drawStep();
    });
    function setVal(v) { v = +v.toFixed(2); s.calibratedFretMm[f] = v; input.value = v; GMB.markDirty(); drawStep(); }
    function nudge(d) {
      var cur = (s.calibratedFretMm[f] === undefined || s.calibratedFretMm[f] === null) ? +theo.toFixed(2) : s.calibratedFretMm[f];
      setVal(cur + d);
    }
    var calCell = (cal === undefined || cal === null) ? h('span.muted', '—') : h('strong', Number(cal).toFixed(2));
    return h('tr', [
      h('td', String(f)),
      h('td', GMB.noteName(s.openNote + f)),
      h('td', theo.toFixed(2)),
      h('td.fret-cal', [
        h('button.btn.ghost.nudge', { type: 'button', onclick: function () { nudge(-0.5); } }, '−'),
        input,
        h('button.btn.ghost.nudge', { type: 'button', onclick: function () { nudge(0.5); } }, '+'),
        h('span.cal-shown', calCell)
      ]),
      h('td', h('div.fret-actions', [
        GMB.button('Go to this fret', function () { jogToFret(s, i, f, theo); }, 'ghost'),
        GMB.button('Capture position', function () { saveFret(s, i, f); }, 'primary')
      ]))
    ]);
  }

  // Jog/test: move the (mock) motor to the fret's calibrated-or-theoretical mm.
  function jogToFret(s, i, f, theo) {
    var target = (s.calibratedFretMm[f] === undefined || s.calibratedFretMm[f] === null) ? +theo.toFixed(2) : s.calibratedFretMm[f];
    motorPos[i] = target;
    GMB.toast('String ' + (i + 1) + ': moved to fret ' + f + ' (' + target.toFixed(2) + ' mm).', 'ok');
    drawStep();
  }
  // Record the current (mock) motor position as this fret's calibrated value.
  function saveFret(s, i, f) {
    var pos = motorPos[i] !== undefined ? motorPos[i] : GMB.fretTheoreticalMm(s, f);
    s.calibratedFretMm[f] = +pos.toFixed(2);
    GMB.markDirty();
    GMB.toast('Fret ' + f + ' saved at ' + s.calibratedFretMm[f].toFixed(2) + ' mm.', 'ok');
    drawStep();
  }

  // ---- Step 8: Test ---------------------------------------------------------
  function stepTest(body) {
    body.appendChild(h('h3', 'Test'));
    body.appendChild(h('p', 'Fire individual actuators and notes. In normal mode nothing actuates until critical errors are cleared.'));
    var p = GMB.state.profile;
    var testWrap = h('div.toolbar.wrap');
    p.strings.forEach(function (s, i) {
      testWrap.appendChild(GMB.button('Test string ' + (i + 1), function () {
        GMB.api.testNote({ channel: 0, note: s.openNote, velocity: 100, durationMs: 400 })
          .then(function (res) {
            if (res && res.ok === false) { GMB.toast(res.error || 'Instrument not ready.', 'warn'); return; }
            GMB.toast('Tested string ' + (i + 1), 'ok');
          }).catch(function (e) { testErr('Note test failed', e); });
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
    // Servo wiring (matches CALIBRATION.md §4.0): no duplicate PCA board+channel,
    // no direct GPIO clashing with a motor signal or another servo, and no
    // per-string servo pointing at a string that does not exist.
    var pcaSeen = {}, servoGpioSeen = {}, stepperGpios = {};
    p.pins.forEach(function (a) { if (a.gpio >= 0) stepperGpios[a.gpio] = a.signal; });
    (p.servos || []).forEach(function (sv) {
      if (!sv.enabled) return;
      var lbl = 'Servo "' + (sv.function || '?') + '"' +
        (sv.stringIndex >= 0 ? ' (string ' + (sv.stringIndex + 1) + ')' : ' (shared)');
      if (sv.stringIndex >= p.instrument.stringCount) out.push(lbl + ' targets a string that does not exist.');
      if (sv.source === 'gpio') {
        if (sv.gpio < 0) { out.push(lbl + ' has no GPIO assigned.'); return; }
        if (stepperGpios[sv.gpio]) out.push(lbl + ' uses GPIO' + sv.gpio + ' already used by ' + stepperGpios[sv.gpio] + '.');
        if (servoGpioSeen[sv.gpio]) out.push(lbl + ' uses GPIO' + sv.gpio + ' already used by ' + servoGpioSeen[sv.gpio] + '.');
        else servoGpioSeen[sv.gpio] = lbl;
      } else {
        if (sv.channel < 0 || sv.channel > 15) out.push(lbl + ' has an invalid PCA channel (0–15).');
        if (sv.pcaBoard < 0 || sv.pcaBoard > 3) out.push(lbl + ' has an invalid PCA board (0–3).');
        var key = sv.pcaBoard + ':' + sv.channel;
        if (pcaSeen[key]) out.push(lbl + ' shares PCA board ' + sv.pcaBoard + ' channel ' + sv.channel + ' with ' + pcaSeen[key] + '.');
        else pcaSeen[key] = lbl;
      }
    });
    return out;
  };

  GMB.views.wizard = {
    render: render,
    reset: function () {
      step = 0;
      activeStr = 0;
      if (statusConn) { statusConn.close(); statusConn = null; }
    }
  };
})(window);
