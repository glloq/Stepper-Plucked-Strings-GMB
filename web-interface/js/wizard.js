/*
 * wizard.js — first-configuration assistant (spec section 10).
 *
 * Nine steps: Identification -> Board choice -> Automatic pin assignment ->
 * Mechanics per string -> Homing -> Servo calibration -> Note calibration ->
 * Test -> Validation.
 *
 * Disclosure is LOCAL, not a global mode (9.2). Each step shows the few decisions
 * you have to make and parks the fine tuning in a GMB.details block you open on
 * the spot: a global Simplified/Advanced toggle makes the one field you need at
 * the bench unreachable, and doubles every page into two variants to maintain.
 * Safety-relevant fields (the LIMIT endstop) stay in plain sight regardless.
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
    // Five-string open-G, matching instrument-profiles/banjo-5string.json:
    // D G B D, then the re-entrant high fifth string (G4). The preset used to
    // carry four notes, which quietly built a four-string banjo.
    banjo: { notes: [50, 55, 59, 62, 67], maxFret: 22 }
  };
  var GM_PROGRAM = { ukulele: 24, guitar: 24, bass: 33, mandolin: 25, banjo: 105 };
  var TYPE_ID = { ukulele: 0x04, guitar: 0x04, bass: 0x05, mandolin: 0x04, banjo: 0x06 };

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
        // Status is DERIVED from the profile, not from how far the user has
        // clicked. A step you walked past without filling in is not done, and a
        // step you fixed later should stop nagging without being revisited.
        var st = stepStatus(i);
        return h('button.step.st-' + st.state + (i === step ? '.active' : ''),
          { onclick: function () { goto(i); }, title: st.why },
          [h('span.step-num', st.state === 'tested' ? '✓✓'
                            : (st.state === 'valid' ? '✓' : String(i + 1))),
           h('span.step-label', label)]);
      })),
      h('p.muted.stepper-legend',
        'Steps show what the profile says, not where you have been: ' +
        '✓ complete · ✓✓ complete and exercised on the machine · a number means ' +
        'something is still missing (hover it).'),
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

  // ---- per-step status ------------------------------------------------------
  //
  // Three states: `incomplete` (the profile is missing something this step owns),
  // `valid` (nothing missing), `tested` (valid AND exercised against the machine
  // in this session). Navigation stays free — you can always open any step — but
  // a step that is not `valid` gates the DANGEROUS actions inside it, because
  // homing an axis with no HOME pin or plucking with no striker is how mechanisms
  // get broken.
  //
  // "Tested" is deliberately session-scoped and never saved: it means "I saw this
  // work just now", and persisting it would let a stale claim outlive the wiring
  // change that invalidated it.
  var tested = {};
  function markTested(stepIndex) {
    if (tested[stepIndex]) return;
    tested[stepIndex] = true;
    GMB.render();
  }

  function enabledStrings() {
    return (GMB.state.profile.strings || []).filter(function (s) { return s.enabled !== false; });
  }

  function stepStatus(i) {
    var p = GMB.state.profile;
    var missing = stepMissing(i, p);
    if (missing) return { state: 'incomplete', why: missing };
    return tested[i] ? { state: 'tested', why: 'Complete, and exercised on the machine this session.' }
                     : { state: 'valid', why: 'Complete.' };
  }

  // What this step still needs, or null. One reason, the first one — a list of
  // nine problems in a tooltip helps nobody.
  function stepMissing(i, p) {
    var strs = p.strings || [];
    switch (i) {
      case 0:
        if (!p.instrument.name) return 'The instrument has no name.';
        if (p.instrument.stringCount < 1 || p.instrument.stringCount > 6) return 'String count must be 1–6.';
        if (strs.length !== p.instrument.stringCount) return 'The string array does not match the string count.';
        return null;
      case 1:
        if (!p.board.profile) return 'No controller board chosen.';
        if (!boardKnows(p.board.profile)) return 'This board is not one the firmware supports.';
        return pinsFitBoard(p);
      case 2:
        return pinsAssigned(p);
      case 3:
        for (var m = 0; m < strs.length; m++) {
          if (strs[m].enabled === false) continue;
          if (!(strs[m].scaleLengthMm > 0)) return 'String ' + (m + 1) + ' has no scale length.';
          if (!(stepsPerMm(strs[m]) > 0)) return 'String ' + (m + 1) + ' resolves to 0 steps/mm.';
          if (!(strs[m].maxSpeedMmS > 0)) return 'String ' + (m + 1) + ' has no maximum speed.';
        }
        return null;
      case 4:
        for (var hgi = 0; hgi < strs.length; hgi++) {
          if (strs[hgi].enabled === false) continue;
          if (pinSignalGpio('HOME' + (hgi + 1)) < 0) return 'String ' + (hgi + 1) + ' has no HOME sensor pin.';
        }
        return null;
      case 5:
        for (var sv = 0; sv < strs.length; sv++) {
          if (strs[sv].enabled === false) continue;
          if (!strikerFor(p, sv)) return 'String ' + (sv + 1) + ' has no pluck or strum servo.';
        }
        return null;
      case 6:
        for (var n = 0; n < strs.length; n++) {
          if (strs[n].enabled === false) continue;
          if (!(strs[n].maxFret >= 0)) return 'String ' + (n + 1) + ' has no fret count.';
        }
        return null;
      case 7:
        // The Test step owns no configuration; it is complete once everything it
        // can exercise is configured.
        return stepMissing(4, p) || stepMissing(5, p);
      case 8:
        var problems = GMB.validateProfile(p);
        return problems.length ? problems[0] : null;
    }
    return null;
  }

  function boardKnows(id) {
    var all = GMB.BOARD_PROFILES || [];
    if (!all.length) return true;            // no table loaded: do not cry wolf
    for (var i = 0; i < all.length; i++) if (all[i].identifier === id) return true;
    return false;
  }

  function pinsFitBoard(p) {
    var all = GMB.BOARD_PROFILES || [];
    var b = null;
    for (var i = 0; i < all.length; i++) if (all[i].identifier === p.board.profile) b = all[i];
    if (!b) return null;
    var byGpio = {};
    b.pins.forEach(function (c) { byGpio[c.gpio] = c; });
    for (var k = 0; k < (p.pins || []).length; k++) {
      var a = p.pins[k];
      if (a.gpio < 0) continue;
      var cap = byGpio[a.gpio];
      if (!cap) return a.signal + ' uses GPIO' + a.gpio + ', which does not exist on this board.';
      if (cap.reserved || cap.preference === 'reserved')
        return a.signal + ' uses GPIO' + a.gpio + ', which is reserved on this board.';
    }
    return null;
  }

  function pinsAssigned(p) {
    var seen = {};
    for (var i = 0; i < (p.pins || []).length; i++) {
      var a = p.pins[i];
      if (a.gpio < 0) return a.signal + ' has no GPIO assigned.';
      if (seen[a.gpio]) return 'GPIO' + a.gpio + ' is used by both ' + seen[a.gpio] + ' and ' + a.signal + '.';
      seen[a.gpio] = a.signal;
    }
    var n = p.instrument.stringCount;
    for (var s = 0; s < n; s++) {
      if (p.strings[s] && p.strings[s].enabled === false) continue;
      if (pinSignalGpio('STEP' + (s + 1)) < 0) return 'String ' + (s + 1) + ' has no STEP pin.';
      if (pinSignalGpio('DIR' + (s + 1)) < 0) return 'String ' + (s + 1) + ' has no DIR pin.';
    }
    return null;
  }

  // The servo that actually strikes a string: the plectrum, or the per-string
  // strum servo. Mirrors perStringStrikeIndex() in the firmware.
  function strikerFor(p, stringIndex) {
    return (p.servos || []).filter(function (sv) {
      return sv.enabled && sv.stringIndex === stringIndex &&
             (sv.function === 'pluck' || sv.function === 'strum');
    })[0] || null;
  }

  // A button that refuses to move the machine while its prerequisites are unmet,
  // and says why instead of failing at the device. This is not a substitute for
  // the firmware's own refusal (which is the real guarantee) — it is so the
  // operator is not invited to press it in the first place.
  function guardedButton(label, reason, onClick, cls) {
    if (!reason) return GMB.button(label, onClick, cls);
    var b = GMB.button(label, function () { GMB.toast(reason, 'warn'); }, 'ghost');
    b.classList.add('blocked');
    b.title = reason;
    return b;
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
      minPositionMm: 0, maxPositionMm: 300, fretOffsetMm: 0, maxSpeedMmS: 200, maxAccelMmS2: 2000,
      calibratedFretMm: [], homing: { direction: -1, fastSpeedMmS: 40, slowSpeedMmS: 5,
        backoffMm: 3, offsetMm: 0, timeoutMs: 8000, maxSearchMm: 500, sensorActiveHigh: true }
    };
  }

  // ---- Step 2: Board --------------------------------------------------------
  // The board list comes from GET /api/boards (offline: the generated tables), so
  // it can only ever offer boards the firmware supports. It used to be a literal
  // one-entry list while the firmware carried four profiles and PlatformIO built
  // three targets.
  var boardList = null;

  function stepBoard(body) {
    var p = GMB.state.profile;
    body.appendChild(h('h3', 'Choose the controller board'));
    if (!boardList) {
      body.appendChild(h('div.card', 'Loading the supported boards…'));
      GMB.api.listBoards().then(function (bs) { boardList = bs; drawStep(); })
        .catch(function () { boardList = []; drawStep(); });
      return;
    }
    var options = boardList.length
      ? boardList.map(function (b) { return { value: b.identifier, label: b.displayName }; })
      : [{ value: p.board.profile, label: p.board.profile }];

    // The board name is long and it is the step's primary control, so give it two
    // grid columns rather than let the select truncate it mid-word.
    var boardField = GMB.field('Board model', GMB.input(p.board, 'profile', {
      type: 'select', options: options, onChange: onBoardChange
    }), 'Changing this re-checks every pin against the new board.');
    boardField.classList.add('wide');
    body.appendChild(h('div.form-grid', [
      boardField,
      GMB.field('Reserve GPIO19/20 for future USB', GMB.input(p.board, 'reserveUsb', { type: 'checkbox' })),
      GMB.field('Automatic pin assignment', GMB.input(p.board, 'automaticPinAssignment', { type: 'checkbox' }))
    ]));
    body.appendChild(boardNotes(p.board.profile));
    body.appendChild(h('div#board-pin-check'));

    // Wi-Fi is a property of the DEVICE, not of the instrument being built, and it
    // is edited (and applied) in Settings. It used to be duplicated here — two
    // editors for one setting, one of which could not apply anything — along with
    // a "Static IP" checkbox for a field that was removed from the schema in v2
    // and drove no WiFi.config() call even before that.
    body.appendChild(h('div.card', [
      h('div.card-head', [h('h3', 'Network'), h('span.muted', 'a device setting, not an instrument one')]),
      h('p.muted', 'Wi-Fi mode, SSID, hostname and passwords live in Settings → Network, ' +
        'where they can also be applied without a reboot. They stay with the machine ' +
        'when you load another instrument.'),
      h('div.toolbar', [GMB.button('Open network settings', function () {
        GMB.openSettings('network');
      }, 'ghost')])
    ]));

    checkBoardPins();
  }

  // Notes derived from the board's own capability table rather than a hard-coded
  // S3 list — the previous text was simply wrong on a classic ESP32.
  function boardNotes(identifier) {
    var b = null, all = GMB.BOARD_PROFILES || [];
    for (var i = 0; i < all.length; i++) if (all[i].identifier === identifier) b = all[i];
    if (!b) return h('div');
    function list(pred) {
      return b.pins.filter(pred).map(function (p) { return p.gpio; });
    }
    var strap = list(function (p) { return p.strapping; });
    var usb = list(function (p) { return p.usb; });
    var onboard = list(function (p) { return p.onboardPeripheral; });
    var reserved = list(function (p) { return p.reserved && !p.strapping && !p.usb && !p.onboardPeripheral; });
    var inputOnly = list(function (p) { return p.input && !p.output; });
    var items = [];
    function add(label, gpios) {
      if (gpios.length) items.push(h('li', label + ': GPIO' + gpios.join(', ')));
    }
    add('Strapping pins (level sampled at reset)', strap);
    add('Native USB / JTAG', usb);
    add('On-board peripherals (UART, LED)', onboard);
    add('Not available (flash, absent, or unusable)', reserved);
    add('Input only — cannot drive STEP/DIR/ENABLE', inputOnly);
    return h('div.note-box', [h('strong', b.displayName + ':'), h('ul', items)]);
  }

  // Changing the board invalidates the pin map: a GPIO that is recommended on the
  // S3 may not exist on a WROOM-32. Re-validate at once and say so, instead of
  // letting the operator reach the Validation step and wonder.
  function onBoardChange() {
    if (GMB.views.pins && GMB.views.pins.reset) GMB.views.pins.reset();  // drop the cached board
    GMB.markDirty();
    drawStep();
  }

  function checkBoardPins() {
    var box = document.getElementById('board-pin-check');
    if (!box) return;
    box.innerHTML = '';
    GMB.api.validatePins(GMB.state.profile).then(function (res) {
      renderBoardPinCheck(box, (res && res.issues) || []);
    }).catch(function (e) {
      var body = e && e.body;
      renderBoardPinCheck(box, (body && body.issues) || []);
    });
  }

  function renderBoardPinCheck(box, issues) {
    box.innerHTML = '';
    if (!issues.length) {
      box.appendChild(h('div.pill.ok', 'Every assigned pin exists and is usable on this board.'));
      return;
    }
    box.appendChild(h('div.pill.error', issues.length + ' pin(s) do not fit this board.'));
    box.appendChild(h('ul.problem-list', issues.slice(0, 8).map(function (is) {
      return h('li', (is.field ? is.field + ' — ' : '') + is.message);
    })));
    box.appendChild(h('div.toolbar', [
      GMB.button('Re-assign pins for this board', function () {
        var p = GMB.state.profile;
        GMB.api.autoPins({ board: p.board.profile, stringCount: p.instrument.stringCount,
                           reserveUsb: p.board.reserveUsb })
          .then(function (res) {
            p.pins = res.pins;
            GMB.markDirty();
            if (res.errors && res.errors.length) {
              GMB.toast(res.errors[0].reason || 'Some signals could not be placed.', 'warn');
            } else {
              GMB.toast('Pins re-assigned for ' + p.board.profile + '.', 'ok');
            }
            drawStep();
          });
      }, 'primary')
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
      GMB.button('Open full pin editor', function () { GMB.navigate('hardware'); }, 'ghost')
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
    // Transmission geometry and travel limits: set once per machine, then never
    // touched again. Parked behind a disclosure so the six decisions above stay
    // the visible ones, with the derived steps/mm in the header as the hint that
    // tells you whether you need to look inside.
    var geometry = GMB.details('mech-geometry-' + i, 'Transmission geometry & travel limits', function () {
      return h('div.form-grid', [
        GMB.field('Steps / revolution', GMB.input(s, 'stepsPerRevolution', { type: 'number', onChange: function () { drawStep(); } })),
        GMB.field('Microsteps', GMB.input(s, 'microsteps', { type: 'number', onChange: function () { drawStep(); } })),
        s.transmission === 'screw'
          ? GMB.field('Lead / rev (mm)', GMB.input(s, 'leadPerRevolutionMm', { type: 'number', onChange: function () { drawStep(); } }))
          : (s.transmission === 'custom'
            ? GMB.field('Custom steps/mm', GMB.input(s, 'customStepsPerMm', { type: 'number', onChange: function () { drawStep(); } }))
            : [GMB.field('Pulley teeth', GMB.input(s, 'pulleyTeeth', { type: 'number', onChange: function () { drawStep(); } })),
               GMB.field('Belt pitch (mm)', GMB.input(s, 'beltPitchMm', { type: 'number', onChange: function () { drawStep(); } }))]),
        GMB.field('Min position (mm)', GMB.input(s, 'minPositionMm', { type: 'number', onChange: function () { drawStep(); } })),
        GMB.field('Max position (mm)', GMB.input(s, 'maxPositionMm', { type: 'number' }),
          'Hard clamp on every move, including a jog.')
      ]);
    }, { hint: 'steps/mm = ' + spm.toFixed(2) });

    var mp = motorPos[i] !== undefined ? motorPos[i] : 0;
    body.appendChild(h('div.substring', [
      // steps/mm is not repeated here: it is the geometry block's hint, right
      // where the numbers that produce it live.
      h('div.substring-head', [h('strong', 'String ' + (i + 1)),
        h('span.pill.mini', GMB.noteName(s.openNote))]),
      h('div.form-grid', basic),
      geometry,
      h('div.toolbar.wrap', [
        h('span.muted', 'Jog axis (check direction):'),
        guardedButton('−5', jogBlockedReason(i), function () { jogAxis(i, -5); }, 'ghost'),
        guardedButton('−1', jogBlockedReason(i), function () { jogAxis(i, -1); }, 'ghost'),
        guardedButton('+1', jogBlockedReason(i), function () { jogAxis(i, 1); }, 'ghost'),
        guardedButton('+5', jogBlockedReason(i), function () { jogAxis(i, 5); }, 'ghost'),
        h('span.motor-pos', { id: 'motor-pos-live' }, 'Motor: ' + mp.toFixed(2) + ' mm')
      ]),
      h('div.toolbar', [copyToAllBtn('Copy mechanics to all strings', copyMechToAll, s)])
    ]));
  }

  // Nudge one axis a few mm (real move on hardware; the live readout updates from
  // the status socket). Confirms motor direction / wiring polarity on the bench.
  // The jog acts on the RUNNING instrument, so it is only meaningful once the
  // edited profile has been saved & activated (otherwise the draft axis index may
  // not match the active one). We gate on the dirty flag and poll the real outcome.
  // A jog with no STEP/DIR pin cannot move anything, and with no HOME pin the
  // axis has no reference — the firmware refuses both, but there is no reason to
  // invite the press.
  function jogBlockedReason(i) {
    if (pinSignalGpio('STEP' + (i + 1)) < 0) return 'String ' + (i + 1) + ' has no STEP pin assigned.';
    if (pinSignalGpio('DIR' + (i + 1)) < 0) return 'String ' + (i + 1) + ' has no DIR pin assigned.';
    if (pinSignalGpio('HOME' + (i + 1)) < 0) return 'String ' + (i + 1) + ' has no HOME sensor — the axis has no reference to jog from.';
    return null;
  }

  function jogAxis(i, deltaMm) {
    if (GMB.state && GMB.state.dirty) {
      GMB.toast('Save & activate the profile first — jog moves the running instrument.', 'warn');
      return;
    }
    GMB.api.jog({ axis: i, deltaMm: deltaMm }).then(function (res) {
      if (res && res.ok === false) { GMB.toast('Jog refused: ' + (res.error || 'instrument not ready (home first)') + '.', 'warn'); return; }
      var id = res && res.commandId;
      if (!id) { jogToast(i, deltaMm); return; }
      pollJog(i, deltaMm, id, 6);
    }).catch(function (e) { testErr('Jog failed', e); });
  }
  function jogToast(i, deltaMm) {
    GMB.toast('String ' + (i + 1) + ': jog ' + (deltaMm > 0 ? '+' : '') + deltaMm + ' mm.', 'ok');
    markTested(3);   // the axis really moved: the Mechanics step is exercised
  }
  // Poll the queued jog for its real loop-side outcome so a refusal (axis busy /
  // faulted / a note playing / not homed) is surfaced instead of a false success.
  function pollJog(i, deltaMm, id, tries) {
    GMB.api.commandState(id).then(function (r) {
      var st = r && r.state;
      if (st === 'succeeded') { jogToast(i, deltaMm); return; }
      if (st === 'refused') { GMB.toast('Jog refused (axis busy, faulted, not homed, or a note is playing).', 'warn'); return; }
      if (tries > 0) setTimeout(function () { pollJog(i, deltaMm, id, tries - 1); }, 120);
      else GMB.toast('String ' + (i + 1) + ': jog queued.', 'ok');
    }).catch(function () { jogToast(i, deltaMm); });
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
      // Caution pins are offered here too (see pins.js): hiding them would leave
      // a classic ESP32 with too few selectable pins to wire an instrument.
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
    body.appendChild(h('p.muted', 'Each string homes on its own HOME switch. Pick the GPIO and the homing behaviour; a LIMIT switch is optional but strongly recommended — without it, a missed HOME sensor is only caught by the search-distance timeout, after the carriage has run to the end of its travel.'));
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
        'Where the axis rests past the HOME sensor (the FDC position).'),
      // The LIMIT endstop stays in plain sight. It is the switch that stops a
      // runaway carriage, so burying it behind a disclosure would be exactly the
      // wrong thing to hide — the intro paragraph above urges fitting one.
      GMB.field('LIMIT switch GPIO (optional)',
        gpioSelect('limit', limitGpio, usedGpios({ exceptSignal: limitSignal }),
          function (g) { setPinSignal(limitSignal, 'limit', g); drawStep(); }),
        'End-of-travel safety switch — strongly recommended.'),
      GMB.field('LIMIT active level', GMB.input(hm, 'limitActiveHigh', {
        type: 'select', options: [{ value: false, label: 'Active low' }, { value: true, label: 'Active high' }],
        coerce: function (v) { return v === 'true' || v === true; }
      }))
    ];
    // Seek tuning: the defaults work on a normal machine, and these only get
    // touched when homing is unreliable or too slow.
    var seek = GMB.details('homing-seek-' + i, 'Homing seek tuning', function () {
      return h('div.form-grid', [
        GMB.field('Fast speed (mm/s)', GMB.input(hm, 'fastSpeedMmS', { type: 'number' }),
          'First approach, until HOME first triggers.'),
        GMB.field('Slow speed (mm/s)', GMB.input(hm, 'slowSpeedMmS', { type: 'number' }),
          'Second approach after the back-off — this one sets the accuracy.'),
        GMB.field('Backoff (mm)', GMB.input(hm, 'backoffMm', { type: 'number' })),
        GMB.field('Timeout (ms)', GMB.input(hm, 'timeoutMs', { type: 'number' })),
        GMB.field('Max search (mm)', GMB.input(hm, 'maxSearchMm', { type: 'number' }),
          'How far to travel before giving up on a missed sensor.')
      ]);
    }, { hint: hm.fastSpeedMmS + ' / ' + hm.slowSpeedMmS + ' mm/s, back-off ' + hm.backoffMm + ' mm' });
    var readout = h('div.endstop-readout', h('span.muted', 'Press “Test endstop” to read the live level.'));
    body.appendChild(h('div.substring', [
      h('div.substring-head', [h('strong', 'String ' + (i + 1) + ' homing'),
        h('span.pill.mini', GMB.noteName(s.openNote)),
        homeGpio < 0 ? h('span.pill.mini.error', 'no HOME pin') : h('span.pill.mini.ok', 'HOME GPIO' + homeGpio)]),
      h('div.form-grid', fields),
      seek,
      h('div.toolbar.wrap', [
        guardedButton('Test endstop',
          pinSignalGpio('HOME' + (i + 1)) < 0 ? 'String ' + (i + 1) + ' has no HOME sensor pin to read.' : null,
          function () { testEndstop(i, s, readout); }, 'ghost'),
        copyToAllBtn('Copy homing to all strings', copyHomingToAll, s)
      ]),
      readout
    ]));
    // Homing drives every carriage at speed toward its endstop. Refuse to even
    // offer it while an axis has no HOME sensor: the only thing that would stop
    // that carriage is the search-distance timeout, at the end of its travel.
    var homeBlocked = stepMissing(4, GMB.state.profile);
    body.appendChild(h('div.toolbar', [guardedButton('Home all axes now', homeBlocked, function () {
      GMB.api.resetSystem().then(function (res) {
        if (res && res.ok === false) {
          GMB.toast('Homing refused: ' + (res.error || 'E-stop/LIMIT active or invalid config') + '.', 'warn');
        } else {
          GMB.toast('Homing started on all axes.', 'ok');
          markTested(4);
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
    damper: 'Damper', sharedDamper: 'Shared damper', aux: 'Auxiliary'
  };
  function roleLabel(fn) { return ROLE_LABEL[fn] || fn; }

  function stepServos(body) {
    body.appendChild(h('h3', 'Servos per string'));
    body.appendChild(h('p.muted', 'Add the servos each string uses. Every servo is driven either by a PCA9685 channel or directly from a free ESP32 GPIO — mix them freely, or use no PCA at all.'));
    body.appendChild(channelMap());
    body.appendChild(stringTabs());
    body.appendChild(stringServoSection(activeStr));
    // Shared / auxiliary servos are rare (most instruments have none), so they
    // fold away — but they are always reachable, which is the point of the whole
    // disclosure scheme. Opened by default when the instrument actually has some.
    var shared = (GMB.state.profile.servos || [])
      .filter(function (sv) { return sv.stringIndex === -1; });
    body.appendChild(GMB.details('servos-shared', 'Shared / auxiliary servos',
      sharedServoSection,
      { open: shared.length > 0,
        hint: shared.length ? shared.length + ' configured' : 'none — a shared damper or auxiliary actuator' }));
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
      h('div.substring-head', [h('strong', 'Shared / auxiliary servos'), h('span.muted', 'stringIndex −1 (a shared damper or auxiliary actuator)')]),
      servos.length ? h('div.servo-list', servos.map(servoRow)) : h('p.muted', 'No shared servo.'),
      h('div.toolbar.wrap', [
        GMB.button('+ Shared damper', function () { addServo('sharedDamper', -1); }, 'ghost'),
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
    if (fn === 'pluck' || fn === 'strum') { opts.activeUs = 1700; opts.travelMs = 90; opts.settleMs = 20; }
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
      markTested(5);
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
    var isStriker = role === 'strum' || role === 'pluck';
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
    // Timing and pulse envelope: set once when the servo is fitted. The key is
    // the servo's identity, not its index, so adding or removing a servo does not
    // reshuffle which blocks are open.
    var key = 'servo-' + sv.stringIndex + '-' + sv.function + '-' +
              (sv.source === 'gpio' ? 'g' + sv.gpio : 'p' + sv.pcaBoard + 'c' + sv.channel);
    var tuning = GMB.details(key, 'Timing & pulse envelope', function () {
      return h('div.form-grid', [
        GMB.field('Function', GMB.input(sv, 'function', {
          type: 'select', options: ['finger', 'pluck', 'strum', 'strumLift', 'damper', 'sharedDamper', 'aux'],
          onChange: function () { drawStep(); }
        }), 'What this servo does — changing it changes how the firmware drives it.'),
        GMB.field('Pulse min (µs)', GMB.input(sv, 'pulseMinUs', { type: 'number' })),
        GMB.field('Pulse max (µs)', GMB.input(sv, 'pulseMaxUs', { type: 'number' }),
          'Hard limits: rest/active are clamped to this window.'),
        GMB.field('Inverted', GMB.input(sv, 'inverted', { type: 'checkbox' })),
        GMB.field('Travel (ms)', GMB.input(sv, 'travelMs', { type: 'number' })),
        GMB.field('Settle (ms)', GMB.input(sv, 'settleMs', { type: 'number' })),
        GMB.field('Disable at rest', GMB.input(sv, 'disableAtRest', { type: 'checkbox' }),
          'Stop the pulse once at rest — the servo stops buzzing and drawing current.')
      ]);
    }, { hint: sv.travelMs + ' ms travel · ' + sv.pulseMinUs + '–' + sv.pulseMaxUs + ' µs' });

    return h('div.servo-row', [
      head,
      h('div.form-grid', fields),
      tuning,
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

  // ---- Step 7: Notes / fret calibration -------------------------------------
  //
  // Calibration is a SEQUENCE, and the UI is now shaped like one. The old screen
  // was a table of every fret with a move and a capture button on each row: on a
  // 6×20 guitar that is 120 rows and 240 buttons, all equally prominent, with
  // nothing showing where you were or whether the last capture was sane. One
  // fret at a time, with theory / measured / Δ side by side, is both fewer
  // decisions per moment and the only layout that makes a bad Δ obvious.
  //
  // The table is still there, folded away, because bulk editing and reviewing a
  // finished calibration are real tasks that a one-at-a-time flow is bad at.
  var calFret = {};        // per string: which fret the assistant is on

  function stepNotes(body) {
    body.appendChild(h('h3', 'Fret positions per string'));
    body.appendChild(h('p.muted', 'Auto-fill the theoretical positions, then measure the frets that matter. A calibrated value always overrides theory in the firmware.'));
    body.appendChild(stringTabs());
    var i = activeStr, s = GMB.state.profile.strings[i];
    if (!s) return;
    var mp = motorPos[i] !== undefined ? motorPos[i] : 0;
    if (s.fretOffsetMm === undefined) s.fretOffsetMm = 0;
    body.appendChild(h('div.substring', [
      h('div.substring-head', [h('strong', 'String ' + (i + 1)),
        GMB.field('Open note (MIDI)', GMB.input(s, 'openNote', { type: 'number', min: 0, max: 127, onChange: function () { drawStep(); } })),
        GMB.field('Max frets', GMB.input(s, 'maxFret', { type: 'number', min: 0, max: 30, onChange: function () { drawStep(); } })),
        h('span.pill.mini', GMB.noteName(s.openNote)),
        h('span.motor-pos', { id: 'motor-pos-live' }, 'Motor: ' + mp.toFixed(2) + ' mm')]),
      h('div.form-grid', [
        GMB.field('Fret offset from FDC (mm)', GMB.input(s, 'fretOffsetMm', { type: 'number', step: '0.1', onChange: function () { drawStep(); } }),
          'Distance from the HOME endstop (FDC) to fret 0 (the nut). Shifts every fret of this string.')
      ]),
      h('div.toolbar.wrap', [
        GMB.button('Auto-fill (theoretical)', function () { autoFill(s); }, 'primary'),
        GMB.button('Clear calibration', function () { s.calibratedFretMm = []; GMB.markDirty(); drawStep(); }, 'ghost'),
        copyToAllBtn('Copy scale + calibration to all', copyFretsToAll, s)
      ]),
      calibrationAssistant(s, i),
      GMB.details('fret-table-' + i, 'All frets (table)', function () { return fretEditor(s, i); },
        { hint: calibratedCount(s) + ' of ' + (s.maxFret + 1) + ' measured' })
    ]));
  }

  function calibratedCount(s) {
    var n = 0;
    for (var f = 0; f <= s.maxFret; f++) {
      var v = s.calibratedFretMm[f];
      if (v !== undefined && v !== null) n++;
    }
    return n;
  }

  // One fret at a time: move → let it settle → capture → next.
  function calibrationAssistant(s, i) {
    var f = calFret[i];
    if (f === undefined || f > s.maxFret) f = calFret[i] = 0;
    var theo = GMB.fretTheoreticalMm(s, f);
    var cal = s.calibratedFretMm[f];
    var measured = (cal === undefined || cal === null) ? null : Number(cal);
    var target = GMB.fretAbsoluteMm(s, f);
    var live = motorPos[i];

    // Δ against theory is the number that tells you whether the capture was sane.
    // A few tenths is normal mechanical reality; several millimetres means the
    // carriage was not where you thought, and catching that HERE is the whole
    // point of showing it next to the value.
    var delta = measured === null ? null : measured - theo;
    var deltaPill;
    if (delta === null) {
      deltaPill = h('span.pill.mini', 'not measured');
    } else {
      var mag = Math.abs(delta);
      var cls = mag > 3 ? 'error' : (mag > 1 ? 'warn' : 'ok');
      deltaPill = h('span.pill.mini.' + cls,
        'Δ ' + (delta >= 0 ? '+' : '') + delta.toFixed(2) + ' mm vs theory');
    }

    function step(d) {
      var next = f + d;
      if (next < 0 || next > s.maxFret) return;
      calFret[i] = next;
      drawStep();
    }

    var dots = [];
    for (var k = 0; k <= s.maxFret; k++) {
      (function (idx) {
        var v = s.calibratedFretMm[idx];
        var done = v !== undefined && v !== null;
        dots.push(h('button.caldot' + (idx === f ? '.active' : '') + (done ? '.done' : ''),
          { type: 'button', title: 'Fret ' + idx + (done ? ' — measured' : ' — not measured'),
            onclick: function () { calFret[i] = idx; drawStep(); } },
          String(idx)));
      })(k);
    }

    return h('div.cal-assist', [
      h('div.cal-head', [
        h('div.cal-fret', [h('span.cal-fret-num', String(f)),
          h('span.cal-fret-note', GMB.noteName(s.openNote + f))]),
        h('div.cal-readouts', [
          readout('Theory (nut)', theo.toFixed(2) + ' mm'),
          readout('Target (from FDC)', target.toFixed(2) + ' mm'),
          readout('Measured (nut)', measured === null ? '—' : measured.toFixed(2) + ' mm'),
          readout('Live motor', live === undefined ? 'no signal' : Number(live).toFixed(2) + ' mm')
        ]),
        deltaPill
      ]),
      h('div.toolbar.wrap', [
        GMB.button('Move to fret ' + f, function () { jogToFret(s, i, f); }, 'ghost'),
        GMB.button('−0.5', function () { nudgeAxis(i, -0.5); }, 'ghost'),
        GMB.button('+0.5', function () { nudgeAxis(i, 0.5); }, 'ghost'),
        GMB.button('Capture position', function () { saveFret(s, i, f); }, 'primary'),
        h('span.spacer'),
        GMB.button('Use theory here', function () {
          s.calibratedFretMm[f] = +theo.toFixed(2);
          GMB.markDirty(); drawStep();
        }, 'ghost')
      ]),
      h('div.toolbar.wrap', [
        GMB.button('◀ Previous', function () { step(-1); }, 'ghost'),
        GMB.button('Next fret ▶', function () { step(1); }, 'primary'),
        h('span.spacer'),
        h('span.muted', calibratedCount(s) + ' of ' + (s.maxFret + 1) + ' frets measured')
      ]),
      h('div.caldots', dots)
    ]);
  }

  function readout(label, value) {
    return h('div.cal-readout', [h('span.cal-label', label), h('span.cal-value', value)]);
  }

  // A fine nudge during calibration. Same endpoint as the Mechanics-step jog; it
  // exists here so the operator never has to leave the fret they are measuring.
  function nudgeAxis(i, mm) {
    GMB.api.jog({ axis: i, deltaMm: mm }).then(function (res) {
      if (res && res.ok === false) GMB.toast('Jog refused: ' + (res.error || 'axis not ready') + '.', 'warn');
    }).catch(function (e) { testErr('Jog failed', e); });
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
      h('thead', h('tr', [h('th', 'Fret'), h('th', 'Note'), h('th', 'Theory mm (nut)'), h('th', 'Calibrated mm (nut)'), h('th', 'Abs (FDC) mm'), h('th', 'Move / save')])),
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
      h('td', GMB.fretAbsoluteMm(s, f).toFixed(2)),
      h('td', h('div.fret-actions', [
        GMB.button('Go to this fret', function () { jogToFret(s, i, f); }, 'ghost'),
        GMB.button('Capture position', function () { saveFret(s, i, f); }, 'primary')
      ]))
    ]);
  }

  // Move the axis to the fret's ABSOLUTE position from the FDC (offset + value).
  // This used to only write motorPos[i] and toast "moved" — nothing on the
  // machine moved, and the operator then pressed "Capture position" to record a
  // number the carriage had never reached. It now issues the real move and only
  // reports success once the device accepted it.
  function jogToFret(s, i, f) {
    var abs = GMB.fretAbsoluteMm(s, f);
    GMB.api.moveTo({ axis: i, positionMm: abs }).then(function (res) {
      if (res && res.ok === false) {
        GMB.toast('Move refused: ' + (res.error || 'the axis is not ready') + '.', 'warn');
        return;
      }
      GMB.toast('String ' + (i + 1) + ': moving to fret ' + f + ' (' + abs.toFixed(2) +
                ' mm from FDC). Wait for it to settle before capturing.', 'ok');
    }).catch(function (e) { testErr('Move failed', e); });
  }
  // Record the live motor position (absolute from FDC) as this fret's calibrated
  // value, stored nut-relative (subtract the per-string fret offset).
  function saveFret(s, i, f) {
    // No live reading means no measurement. Falling back to the theoretical
    // position here would store theory while claiming it was captured from the
    // machine — the one thing this whole step exists to avoid.
    if (motorPos[i] === undefined) {
      GMB.toast('No live position for string ' + (i + 1) + ' — the device is not ' +
                'reporting. Nothing captured.', 'warn');
      return;
    }
    var abs = motorPos[i];
    s.calibratedFretMm[f] = +(abs - (s.fretOffsetMm || 0)).toFixed(2);
    GMB.markDirty();
    GMB.toast('Fret ' + f + ' captured at ' + abs.toFixed(2) + ' mm from FDC.', 'ok');
    markTested(6);   // a real measurement landed in the profile
    drawStep();
  }

  // ---- Step 8: Test ---------------------------------------------------------
  function stepTest(body) {
    body.appendChild(h('h3', 'Test'));
    body.appendChild(h('p', 'Fire individual actuators and notes. In normal mode nothing actuates until critical errors are cleared.'));
    var p = GMB.state.profile;
    var testWrap = h('div.toolbar.wrap');
    p.strings.forEach(function (s, i) {
      // Playing a note moves a carriage AND fires a striker. A string with no
      // HOME reference or no striker cannot do that, so say so rather than let
      // the operator hunt for why nothing happened.
      var blocked = s.enabled === false ? 'String ' + (i + 1) + ' is disabled.'
        : (pinSignalGpio('HOME' + (i + 1)) < 0 ? 'String ' + (i + 1) + ' has no HOME sensor pin.'
          : (!strikerFor(p, i) ? 'String ' + (i + 1) + ' has no pluck or strum servo.' : null));
      testWrap.appendChild(guardedButton('Test string ' + (i + 1), blocked, function () {
        GMB.api.testNote({ channel: p.midi.globalChannel | 0, note: s.openNote,
                           velocity: 100, durationMs: 400 })
          .then(function (res) {
            if (res && res.ok === false) { GMB.toast(res.error || 'Instrument not ready.', 'warn'); return; }
            GMB.toast('Tested string ' + (i + 1), 'ok');
            markTested(7);
          }).catch(function (e) { testErr('Note test failed', e); });
      }, 'ghost'));
    });
    testWrap.appendChild(guardedButton('Test chord (all open strings)',
      stepMissing(7, p), function () { testChord(p); }, 'ghost'));
    testWrap.appendChild(GMB.button('STOP', GMB.doPanic, 'danger'));
    body.appendChild(testWrap);
    body.appendChild(h('p.muted', 'Full note/string/fret testing with a step trace lives on the MIDI page.'));
    body.appendChild(GMB.button('Open MIDI test tool', function () { GMB.openSettings('tools'); }, 'primary'));
  }

  // Strum every enabled string open. This used to be a bare toast claiming a
  // chord had been sent while nothing was emitted at all. Notes go out spread by
  // a few ms — the same shape the chord grouping window expects — and the result
  // is reported from what the device actually accepted.
  function testChord(p) {
    var picks = [];
    (p.strings || []).forEach(function (s, i) {
      if (s.enabled !== false) picks.push({ index: i, note: s.openNote });
    });
    if (!picks.length) { GMB.toast('No enabled string to strum.', 'warn'); return; }
    var refused = 0, done = 0;
    picks.forEach(function (pk, n) {
      setTimeout(function () {
        GMB.api.testNote({ channel: p.midi.globalChannel | 0, note: pk.note,
                           velocity: 96, durationMs: 600 })
          .then(function (res) { if (res && res.ok === false) refused++; })
          .catch(function () { refused++; })
          .then(function () {
            if (++done < picks.length) return;
            if (refused === picks.length)
              GMB.toast('Chord refused — the instrument is not ready.', 'warn');
            else if (refused)
              GMB.toast('Chord sent, ' + refused + ' of ' + picks.length +
                        ' strings refused.', 'warn');
            else
              GMB.toast('Chord sent on ' + picks.length + ' strings.', 'ok');
          });
      }, n * 12);
    });
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
        if (sv.pcaBoard < 0 || sv.pcaBoard > 7) out.push(lbl + ' has an invalid PCA board (0–7 = 0x40–0x47).');
        var bus = sv.i2cBus === 1 ? 1 : 0;
        if (sv.i2cBus !== undefined && sv.i2cBus !== 0 && sv.i2cBus !== 1)
          out.push(lbl + ' has an invalid I²C bus (0 or 1).');
        // A board is identified by (bus, address): the same address on the OTHER
        // bus is a different chip, so the bus is part of the clash key.
        var key = bus + ':' + sv.pcaBoard + ':' + sv.channel;
        if (pcaSeen[key]) out.push(lbl + ' shares I²C bus ' + bus + ' board ' + sv.pcaBoard + ' channel ' + sv.channel + ' with ' + pcaSeen[key] + '.');
        else pcaSeen[key] = lbl;
      }
    });
    return out;
  };

  GMB.views.wizard = {
    render: render,
    // Jump to a step by index (0-based) or by its label. Used by the setup flow
    // itself and by web-interface/tools/screenshots.js, which needs to walk every
    // step to capture it — without a hook it would have to click through the
    // stepper and guess when each render settled.
    goto: function (which) {
      var i = typeof which === 'number' ? which : STEPS.indexOf(which);
      if (i < 0 || i >= STEPS.length) return false;
      goto(i);
      return true;
    },
    steps: function () { return STEPS.slice(); },
    // Which string the per-string steps (Mechanics, Homing, Servos, Notes) show.
    selectString: function (i) { activeStr = i | 0; drawStep(); },
    reset: function () {
      step = 0;
      activeStr = 0;
      if (statusConn) { statusConn.close(); statusConn = null; }
    }
  };
})(window);
