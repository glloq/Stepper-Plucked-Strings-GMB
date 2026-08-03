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

  function render(host) {
    if (monitor) { monitor.close(); monitor = null; }
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
        GMB.button('Send a test', function () { GMB.navigate('midi'); scrollToTest(); }, 'ghost')
      ]),
      h('p.muted', mode_desc(sfs.mode))
    ]);
    host.appendChild(simplified);

    // ---- Advanced settings --------------------------------------------------
    if (GMB.isAdvanced()) {
      host.appendChild(advancedPanel(sfs, p));
    }

    // ---- MIDI monitor (section 15) ------------------------------------------
    var monHost = h('div.card', [h('div.card-head', [h('h2', 'MIDI monitor'),
      h('span.muted', 'live received events')]), h('div#monitor-host')]);
    host.appendChild(monHost);
    monitor = GMB.midiMonitor.mount(monHost.querySelector('#monitor-host'));

    // ---- Integrated test tool (section 16) ----------------------------------
    host.appendChild(testTool(p));
  }

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
  var testCfg = { string: 1, fret: 5, note: 64, velocity: 100, channel: 0, durationMs: 400 };

  function testTool(p) {
    testCfg.string = Math.min(testCfg.string, p.instrument.stringCount);
    var log = h('div#test-log.step-log');
    var card = h('div.card#test-tool', [
      h('h2', 'Integrated test tool'),
      h('p.muted', 'Sends CC(string) + CC(fret) + Note On, then Note Off after the chosen duration, and traces each step.'),
      h('div.form-grid', [
        GMB.field('String', GMB.input(testCfg, 'string', { type: 'number', min: 1, max: p.instrument.stringCount })),
        GMB.field('Fret', GMB.input(testCfg, 'fret', { type: 'number', min: 0, max: 30 })),
        GMB.field('MIDI note', GMB.input(testCfg, 'note', { type: 'number', min: 0, max: 127 })),
        GMB.field('Velocity', GMB.input(testCfg, 'velocity', { type: 'number', min: 1, max: 127 })),
        GMB.field('Channel (1–16)', GMB.input(testCfg, 'channel', { type: 'number', min: 0, max: 15 })),
        GMB.field('Note duration (ms)', GMB.input(testCfg, 'durationMs', { type: 'number', min: 20, max: 5000 }))
      ]),
      h('div.toolbar', [
        GMB.button('Send test', function () { runTest(p, log); }, 'primary'),
        GMB.button('Auto-fill note from string+fret', function () {
          testCfg.note = p.strings[testCfg.string - 1].openNote + testCfg.fret;
          GMB.render(); scrollToTest();
        }, 'ghost')
      ]),
      log
    ]);
    return card;
  }

  function runTest(p, log) {
    log.innerHTML = '';
    var sfs = p.stringFretSelection;
    // The firmware reads only channel/note/velocity/durationMs; the extra
    // string/fret/CC fields feed the offline mock's step trace.
    GMB.api.testNote({
      string: testCfg.string, fret: testCfg.fret,
      stringCc: sfs.string.ccNumber, fretCc: sfs.fret.ccNumber,
      note: testCfg.note, velocity: testCfg.velocity, channel: testCfg.channel, durationMs: testCfg.durationMs
    }).then(function (res) {
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
          if (monitor) monitor.push({ channel: testCfg.channel + 1, type: 'noteOff', note: testCfg.note,
            value: 0, interpretation: 'release string ' + testCfg.string, t: Date.now() });
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

  GMB.views.midi = { render: render };
})(window);
