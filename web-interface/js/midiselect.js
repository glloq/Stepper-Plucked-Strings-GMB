/*
 * midiselect.js — MIDI page: string/fret selection (string/fret selection spec)
 * plus the general MIDI parameters (spec 18).
 *
 * Contains: the simplified selection panel (section 14) with the GMB preset
 * button (section 3); advanced settings (CC numbers, min/max/offset, numbering,
 * order + custom mapping, timeout); a real-time MIDI monitor (section 15, via
 * midimonitor.js); and the integrated test tool (section 16) that sends
 * CC+CC+NoteOn+NoteOff and shows each step. The GMB identity & capabilities /
 * SysEx tooling lives on its own tab (see sysex.js).
 */
(function (global) {
  'use strict';
  var GMB = global.GMB, h = GMB.h;

  var monitor = null;

  function renderSettings(host) {
    var p = GMB.state.profile;
    var sfs = p.stringFretSelection;

    // ---- General MIDI parameters (spec 18) --------------------
    host.appendChild(h('div.card', [
      h('h2', 'MIDI parameters'),
      h('div.form-grid', [
        GMB.field('Global channel (1–16)', GMB.input(p.midi, 'globalChannel', {
          type: 'number', min: 0, max: 15,
          onChange: function () {}, // stored zero-based
        }), 'stored zero-based (0 = channel 1)'),
        GMB.field('Omni mode', GMB.input(p.midi, 'omni', { type: 'checkbox' })),
        GMB.field('Transpose (semitones)', GMB.input(p.midi, 'transpose', { type: 'number', min: -24, max: 24 })),
        GMB.field('Chord window (ms)', GMB.input(p.midi, 'chordWindowMs', { type: 'number', min: 0, max: 50 })),
        GMB.field('Velocity curve', GMB.input(p.midi, 'velocityCurve', {
          // 'custom' is intentionally omitted: no custom curve table exists yet,
          // so offering it would silently behave like 'linear' (audit P1-12).
          type: 'select', options: ['linear', 'soft', 'hard', 'exponential']
        })),
        GMB.field('Sustain pedal', GMB.input(p.midi, 'sustainPedal', { type: 'checkbox', onChange: function () { GMB.render(); } })),
        p.midi.sustainPedal
          ? GMB.field('Sustain CC number', GMB.input(p.midi, 'sustainCc', { type: 'number', min: 0, max: 119 }), 'Default 64 (sustain pedal).')
          : null,
        GMB.field('Chord saturation strategy', GMB.input(p.midi, 'saturationStrategy', {
          type: 'select', options: [
            { value: 'priorityLow', label: 'Keep lowest notes' },
            { value: 'priorityHigh', label: 'Keep highest notes' },
            { value: 'priorityFirst', label: 'Keep first-arriving' },
            { value: 'replaceOldest', label: 'Replace oldest' },
            { value: 'ignoreExtra', label: 'Ignore extra notes' },
            { value: 'monophonic', label: 'Monophonic (one note)' }]
        }), 'What happens when more notes arrive than there are strings.')
      ])
    ]));

    // ---- Playback timing / latency -----------------------------------------
    host.appendChild(h('div.card', [
      h('h2', 'Playback timing'),
      h('p.muted', 'Manage the delay between receiving a note and hearing it, and how early the mechanics anticipate.'),
      h('div.form-grid', [
        GMB.field('Note execution delay (ms)', GMB.input(p.midi, 'noteExecutionDelayMs', { type: 'number', min: 0, max: 500 }),
          'Fixed delay from Note On to the note actually sounding, giving the finger a constant window to reach the fret.'),
        GMB.field('Finger lead (ms)', GMB.input(p.midi, 'fingerLeadMs', { type: 'number', min: 0, max: 500 }),
          'Start the finger descent this long before the carriage is estimated to arrive (0 = press only on arrival). Tune to avoid dragging.'),
        GMB.field('Strum lead (ms)', GMB.input(p.midi, 'strumLeadMs', { type: 'number', min: 0, max: 500 }),
          'Start lowering the strum lift this long before the string is ready, so it is engaged when the strike time comes.')
      ])
    ]));

    // ---- Simplified string/fret selection panel (section 14) ----------------
    var simplified = h('div.card', [
      h('div.card-head', [h('h2', 'String / fret selection'),
        h('span.muted', 'General-Midi-Boop tablature over MIDI CC')]),
      h('label.inline.big', [GMB.input(sfs, 'enabled', { type: 'checkbox', onChange: function () { GMB.render(); } }),
        h('span', 'Enable string/fret selection')]),
      h('div.form-grid', [
        GMB.field('System', GMB.input(sfs, 'preset', { type: 'select', options: [
          { value: 'general-midi-boop', label: 'General-Midi-Boop' }, { value: 'custom', label: 'Custom' }] })),
        GMB.field('String CC', GMB.input(sfs.string, 'ccNumber', { type: 'number', min: 0, max: 119 })),
        GMB.field('Fret CC', GMB.input(sfs.fret, 'ccNumber', { type: 'number', min: 0, max: 119 })),
        GMB.field('String numbering', GMB.input(sfs.string, 'numbering', {
          type: 'select', options: [{ value: 'oneBased', label: '1 to N' }, { value: 'zeroBased', label: '0 to N-1' }] })),
        GMB.field('String order', GMB.input(sfs.string, 'reverseOrder', {
          type: 'select', options: [{ value: false, label: 'Normal' }, { value: true, label: 'Reversed' }],
          coerce: function (v) { return v === 'true' || v === true; }, onChange: function () { GMB.render(); } })),
        GMB.field('When CC missing', GMB.input(sfs.validation, 'missingSelectionPolicy', {
          type: 'select', options: [
            { value: 'automaticAllocation', label: 'Choose automatically' },
            { value: 'reject', label: 'Reject' }] }))
      ]),
      h('div.toolbar', [
        GMB.button('Apply General-Midi-Boop preset', applyGmbPreset, 'primary'),
        GMB.button('Send a test', function () { GMB.openSettings('advanced'); }, 'ghost')
      ]),
      h('p.muted', mode_desc(sfs.mode))
    ]);
    host.appendChild(simplified);

    // ---- Advanced settings --------------------------------------------------
    if (GMB.isAdvanced()) {
      host.appendChild(advancedPanel(sfs, p));
    }
  }

  // The LIVE tools (monitor + note tester). Separate from the settings above so
  // opening a settings panel does not silently open a MIDI socket, and so the
  // Settings modal's Advanced tab can host the tools on their own.
  function renderTools(host) {
    if (monitor) { monitor.close(); monitor = null; }
    var p = GMB.state.profile;

    // ---- MIDI monitor (section 15) ------------------------------------------
    var monHost = h('div.card', [h('div.card-head', [h('h2', 'MIDI monitor'),
      h('span.muted', 'live received events')]), h('div#monitor-host')]);
    host.appendChild(monHost);
    monitor = GMB.midiMonitor.mount(monHost.querySelector('#monitor-host'));

    // ---- Integrated test tool (section 16) ----------------------------------
    host.appendChild(testTool(p));
  }

  function render(host) { renderSettings(host); renderTools(host); }

  function mode_desc(mode) {
    return {
      automatic: 'Automatic: the controller ignores CCs and allocates strings itself.',
      explicit: 'Explicit: string & fret are forced by the incoming CCs.',
      hybrid: 'Hybrid (recommended): use CCs when valid, fall back to automatic allocation otherwise.'
    }[mode] || '';
  }

  function applyGmbPreset() {
    var p = GMB.state.profile, sfs = p.stringFretSelection;
    sfs.enabled = true;
    sfs.mode = 'hybrid';
    sfs.preset = 'general-midi-boop';
    sfs.perMidiChannel = true;
    sfs.prepareOnCompleteSelection = true;
    sfs.string.ccNumber = 20;
    sfs.string.minimum = 1;
    sfs.string.maximum = p.instrument.stringCount;
    sfs.string.offset = 0;
    sfs.string.numbering = 'oneBased';
    sfs.string.reverseOrder = false;
    sfs.string.mapping = p.strings.map(function (_, i) { return i; });
    sfs.fret.ccNumber = 21;
    sfs.fret.minimum = 0;
    sfs.fret.maximum = Math.max.apply(null, p.strings.map(function (s) { return s.maxFret; }));
    sfs.fret.offset = 0;
    sfs.validation.missingSelectionPolicy = 'automaticAllocation';
    GMB.markDirty();
    GMB.toast('General-Midi-Boop preset applied.', 'ok');
    GMB.render();
  }

  function advancedPanel(sfs, p) {
    var fields = [
      GMB.field('Selection mode', GMB.input(sfs, 'mode', {
        type: 'select', options: [
          { value: 'automatic', label: 'Automatic' }, { value: 'explicit', label: 'Explicit (CC forced)' },
          { value: 'hybrid', label: 'Hybrid' }], onChange: function () { GMB.render(); } })),
      GMB.field('Per-MIDI-channel selection', GMB.input(sfs, 'perMidiChannel', { type: 'checkbox' })),
      GMB.field('Selection timeout (ms, 5–2000)', GMB.input(sfs, 'selectionTimeoutMs', { type: 'number', min: 5, max: 2000 })),
      GMB.field('Prepare motor on complete selection', GMB.input(sfs, 'prepareOnCompleteSelection', { type: 'checkbox' })),
      GMB.field('Queue depth (≥16)', GMB.input(sfs, 'queueDepth', { type: 'number', min: 16, max: 128 }))
    ];
    var stringFields = [
      GMB.field('String CC', GMB.input(sfs.string, 'ccNumber', { type: 'number', min: 0, max: 119 })),
      GMB.field('Min value', GMB.input(sfs.string, 'minimum', { type: 'number', min: 0, max: 127 })),
      GMB.field('Max value', GMB.input(sfs.string, 'maximum', { type: 'number', min: 0, max: 127 })),
      GMB.field('Offset', GMB.input(sfs.string, 'offset', { type: 'number', min: -64, max: 63 }))
    ];
    var fretFields = [
      GMB.field('Fret CC', GMB.input(sfs.fret, 'ccNumber', { type: 'number', min: 0, max: 119 })),
      GMB.field('Min value', GMB.input(sfs.fret, 'minimum', { type: 'number', min: 0, max: 127 })),
      GMB.field('Max value', GMB.input(sfs.fret, 'maximum', { type: 'number', min: 0, max: 127 })),
      GMB.field('Offset', GMB.input(sfs.fret, 'offset', { type: 'number', min: -64, max: 63 })),
      GMB.field('Invalid value policy', GMB.input(sfs.fret, 'invalidValuePolicy', {
        type: 'select', options: [
          { value: 'reject', label: 'Reject' }, { value: 'clamp', label: 'Clamp to range' },
          { value: 'automaticFallback', label: 'Automatic fallback' }, { value: 'lastValid', label: 'Last valid value' }] }))
    ];
    var validationFields = [
      GMB.field('Note/position policy', GMB.input(sfs.validation, 'notePositionPolicy', {
        type: 'select', options: [
          { value: 'ccPriorityWithWarning', label: 'CC priority (warn on mismatch)' },
          { value: 'notePriority', label: 'Note priority (recompute fret)' },
          { value: 'strict', label: 'Strict (reject incoherent)' }] })),
      GMB.field('Missing selection policy', GMB.input(sfs.validation, 'missingSelectionPolicy', {
        type: 'select', options: [
          { value: 'automaticAllocation', label: 'Automatic allocation' }, { value: 'reject', label: 'Reject' }] })),
      GMB.field('Expired selection policy', GMB.input(sfs.validation, 'expiredSelectionPolicy', {
        type: 'select', options: [
          { value: 'automaticAllocation', label: 'Automatic allocation' }, { value: 'reject', label: 'Reject' }] }))
    ];
    var card = h('div.card', [
      h('h2', 'Advanced selection settings'),
      h('div.form-grid', fields),
      h('h3', 'String selection'),
      h('div.form-grid', stringFields),
      h('h3', 'Fret selection'),
      h('div.form-grid', fretFields),
      h('h3', 'Validation'),
      h('div.form-grid', validationFields),
      h('h3', 'String order mapping'),
      mappingEditor(sfs, p)
    ]);
    return card;
  }

  // Custom logical-value -> physical-axis mapping (selection spec section 6).
  function mappingEditor(sfs, p) {
    var n = p.instrument.stringCount;
    if (!sfs.string.mapping || sfs.string.mapping.length !== n) {
      sfs.string.mapping = [];
      for (var i = 0; i < n; i++) sfs.string.mapping.push(sfs.string.reverseOrder ? (n - 1 - i) : i);
    }
    var rows = [];
    for (var v = 0; v < n; v++) {
      (function (idx) {
        var ccVal = sfs.string.numbering === 'oneBased' ? (idx + 1) : idx;
        var sel = h('select');
        for (var a = 0; a < n; a++) {
          sel.appendChild(h('option', { value: a, selected: sfs.string.mapping[idx] === a },
            'axis ' + (a + 1) + ' (' + GMB.noteName(p.strings[a].openNote) + ')'));
        }
        sel.addEventListener('change', function () { sfs.string.mapping[idx] = Number(sel.value); GMB.markDirty(); });
        rows.push(h('tr', [h('td', 'CC value ' + ccVal), h('td', '→'), h('td', sel)]));
      })(v);
    }
    return h('div.table-wrap', h('table.mini-table', [
      h('thead', h('tr', [h('th', 'MIDI value'), h('th', ''), h('th', 'Physical axis')])),
      h('tbody', rows)
    ]));
  }

  // ---- Integrated test tool (section 16) ------------------------------------
  // The two CC fields hold the VALUES a controller would put on the wire, not a
  // string number the UI silently re-encodes. That is the whole point: the
  // firmware decodes them with the configured numbering / offset / order /
  // mapping, so a wrong mapping shows up as the wrong axis moving instead of
  // being corrected on the way out.
  var testCfg = { ccString: 1, ccFret: 5, sendSelection: true,
                  note: 64, velocity: 100, channel: 0, durationMs: 400 };

  function testTool(p) {
    var sfs = p.stringFretSelection;
    var log = h('div#test-log.step-log');
    var axis = GMB.decodeStringCc(sfs, p, testCfg.ccString);
    var fret = GMB.decodeFretCc(sfs, testCfg.ccFret);
    var resolves = axis < 0
      ? h('span.pill.mini.warn', 'CC ' + testCfg.ccString + ' selects no string')
      : h('span.pill.mini.ok', 'selects axis ' + (axis + 1) +
          (fret >= 0 ? ' · fret ' + fret : ''));
    var fields = [
      GMB.field('Send selection CCs', GMB.input(testCfg, 'sendSelection', {
        type: 'checkbox', onChange: function () { GMB.render(); }
      }), 'Off = plain Note On, allocated automatically.')
    ];
    if (testCfg.sendSelection) {
      fields.push(GMB.field('String CC value (CC' + sfs.string.ccNumber + ')',
        GMB.input(testCfg, 'ccString', { type: 'number', min: 0, max: 127,
          onChange: function () { GMB.render(); } })));
      fields.push(GMB.field('Fret CC value (CC' + sfs.fret.ccNumber + ')',
        GMB.input(testCfg, 'ccFret', { type: 'number', min: 0, max: 127,
          onChange: function () { GMB.render(); } })));
    }
    fields.push(GMB.field('MIDI note', GMB.input(testCfg, 'note', { type: 'number', min: 0, max: 127 })));
    fields.push(GMB.field('Velocity', GMB.input(testCfg, 'velocity', { type: 'number', min: 1, max: 127 })));
    fields.push(GMB.field('Channel (1–16)', GMB.input(testCfg, 'channel', { type: 'number', min: 0, max: 15 })));
    fields.push(GMB.field('Note duration (ms)', GMB.input(testCfg, 'durationMs', { type: 'number', min: 20, max: 5000 })));

    var card = h('div.card#test-tool', [
      h('div.card-head', [h('h2', 'Integrated test tool'),
        testCfg.sendSelection ? resolves : h('span.muted', 'automatic allocation')]),
      h('p.muted', testCfg.sendSelection
        ? 'Sends CC' + sfs.string.ccNumber + ' + CC' + sfs.fret.ccNumber + ' + Note On on ' +
          'the device, then Note Off after the chosen duration. The firmware decodes the ' +
          'CC values with the live selector config, so this tests the real ' +
          'General-Midi-Boop chain end to end.'
        : 'Sends a bare Note On, then Note Off after the chosen duration — the ' +
          'controller allocates the string itself.'),
      h('div.form-grid', fields),
      h('div.toolbar', [
        GMB.button('Send test', function () { runTest(p, log); }, 'primary'),
        GMB.button('Auto-fill note from the selected string+fret', function () {
          var a = GMB.decodeStringCc(sfs, p, testCfg.ccString);
          var f = GMB.decodeFretCc(sfs, testCfg.ccFret);
          if (a < 0 || f < 0) { GMB.toast('Those CC values select nothing.', 'warn'); return; }
          testCfg.note = p.strings[a].openNote + f;
          GMB.render(); scrollToTest();
        }, 'ghost')
      ]),
      log
    ]);
    return card;
  }

  function runTest(p, log) {
    log.innerHTML = '';
    var payload = { note: testCfg.note, velocity: testCfg.velocity,
                    channel: testCfg.channel, durationMs: testCfg.durationMs };
    if (testCfg.sendSelection) {
      payload.ccString = testCfg.ccString;
      payload.ccFret = testCfg.ccFret;
    }
    GMB.api.testNote(payload).then(function (res) {
      if (res && res.ok === false) {
        log.appendChild(h('div.step-line.error', [h('span.step-dot'), h('strong', 'Rejected'),
          h('span.muted', ' — ' + (res.error || 'instrument not ready'))]));
        GMB.toast('Test note rejected: ' + (res.error || 'not ready'), 'warn');
        return;
      }
      // Real firmware returns { ok:true } with no trace; the mock returns steps.
      var steps = res.steps || [{ step: 'Note sent', detail: 'ch ' + (testCfg.channel + 1) +
        ', note ' + testCfg.note + ', vel ' + testCfg.velocity }];
      steps.forEach(function (s, i) {
        setTimeout(function () {
          log.appendChild(h('div.step-line', [h('span.step-dot'), h('strong', s.step),
            s.detail ? h('span.muted', ' — ' + s.detail) : null]));
        }, i * 120);
      });
      // Emit the corresponding Note Off after the chosen duration (mock stream).
      setTimeout(function () {
        GMB.injectMidi && GMB.api.mock && (function () {
          var axis = testCfg.sendSelection
            ? GMB.decodeStringCc(p.stringFretSelection, p, testCfg.ccString) : -1;
          if (monitor) monitor.push({ channel: testCfg.channel + 1, type: 'noteOff', note: testCfg.note,
            value: 0, interpretation: axis >= 0 ? 'release string ' + (axis + 1) : 'release',
            t: Date.now() });
        })();
      }, testCfg.durationMs);
    }).catch(function (e) {
      var msg = (e && e.body && e.body.error) || (e && e.message) || 'error';
      log.appendChild(h('div.step-line.error', [h('span.step-dot'), h('strong', 'Error'),
        h('span.muted', ' — ' + msg)]));
      GMB.toast('Test note failed: ' + msg, 'error');
    });
  }

  function scrollToTest() {
    var el = document.getElementById('test-tool');
    if (el) el.scrollIntoView({ behavior: 'smooth' });
  }

  GMB.views.midi = {
    render: render,
    teardown: function () { if (monitor) { monitor.close(); monitor = null; } }
  };
  // Split entry points for the Settings modal: settings-only (the config wizard's
  // MIDI step) and the live tools (the Advanced tab).
  GMB.midiSettings = {
    settings: renderSettings,
    tools: renderTools,
    teardown: function () { if (monitor) { monitor.close(); monitor = null; } }
  };
})(window);
