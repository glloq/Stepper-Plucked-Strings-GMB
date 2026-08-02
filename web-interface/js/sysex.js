/*
 * sysex.js — MIDI > GMB identity & capabilities (SysEx spec section 17 & 18).
 *
 * Simplified view: name / type / preset / GM program / channel, "Publish
 * capabilities", "Test communication", last detection state, plus the read-only
 * computed capabilities (strings, frets, MIDI range, polyphony, CC string/fret,
 * tuning, revision). Advanced view: enable blocks 5/6/7, block-7 version,
 * polyphony override, announced CCs, raw SysEx bytes, and the integrated SysEx
 * tester (request identity/descriptor/capabilities/string config/notify/full
 * discovery, showing sent + received + decoded fields).
 */
(function (global) {
  'use strict';
  var GMB = global.GMB, h = GMB.h;

  // Non-persisted advanced toggles (would map to firmware options).
  var adv = { block5: true, block6: true, block7: true, block7Version: 1, polyphonyOverride: -1, detectionEnabled: true };
  var lastDetection = 'Never';

  function render(host) {
    var p = GMB.state.profile;
    var caps = GMB.computeCapabilities(p);

    // ---- Simplified identity panel ------------------------------------------
    host.appendChild(h('div.card', [
      h('div.card-head', [h('h2', 'GMB identity & capabilities'),
        h('span.muted', 'announced automatically to General-Midi-Boop')]),
      h('div.form-grid', [
        GMB.field('Enable GMB detection', GMB.input(adv, 'detectionEnabled', { type: 'checkbox' })),
        GMB.field('Instrument name', GMB.input(p.instrument, 'name', { onChange: function () { GMB.render(); } })),
        GMB.field('Instrument type', GMB.input(p.instrument, 'type', {
          type: 'select', options: ['ukulele', 'guitar', 'bass', 'mandolin', 'banjo', 'custom'] })),
        GMB.field('GM program', GMB.input(p.instrument, 'gmProgram', { type: 'number', min: 0, max: 127, onChange: function () { GMB.render(); } })),
        GMB.field('MIDI channel (1–16)', GMB.input(p.midi, 'globalChannel', { type: 'number', min: 0, max: 15 }), 'stored zero-based')
      ]),
      h('div.toolbar', [
        GMB.button('Publish capabilities', publish, 'primary'),
        GMB.button('Test communication', testComms, 'ghost'),
        h('span.muted', 'Last detection: '), h('span#last-detection.pill.mini', lastDetection)
      ])
    ]));

    // ---- Read-only computed capabilities ------------------------------------
    host.appendChild(h('div.card', [
      h('h2', 'Computed capabilities (read-only)'),
      h('div.grid.caps-grid', [
        capItem('Strings', caps.strings),
        capItem('Frets', caps.frets),
        capItem('MIDI range', GMB.noteName(caps.noteMin) + ' – ' + GMB.noteName(caps.noteMax) +
          ' (' + caps.noteMin + '–' + caps.noteMax + ')'),
        capItem('Note mode', caps.noteMode === 0 ? 'continuous' : 'discrete'),
        capItem('Polyphony', adv.polyphonyOverride >= 0 ? adv.polyphonyOverride + ' (override)' : caps.polyphony),
        capItem('CC string', caps.ccString),
        capItem('CC fret', caps.ccFret),
        capItem('Tuning', caps.tuningNames.join(' ')),
        capItem('Capo', caps.capo),
        capItem('Revision', caps.revision)
      ])
    ]));

    // ---- Advanced ------------------------------------------------------------
    if (GMB.isAdvanced()) {
      host.appendChild(h('div.card', [
        h('h2', 'Advanced GMB options'),
        h('div.form-grid', [
          GMB.field('Enable block 5 (descriptor)', GMB.input(adv, 'block5', { type: 'checkbox' })),
          GMB.field('Enable block 6 (capabilities)', GMB.input(adv, 'block6', { type: 'checkbox' })),
          GMB.field('Enable block 7 (string config)', GMB.input(adv, 'block7', { type: 'checkbox' })),
          GMB.field('Block 7 version', GMB.input(adv, 'block7Version', {
            type: 'select', options: [{ value: 1, label: 'v1 (compatible)' }, { value: 2, label: 'v2 (extended)' }], coerce: Number })),
          GMB.field('Polyphony override (−1 = auto)', GMB.input(adv, 'polyphonyOverride', {
            type: 'number', min: -1, max: 6, onChange: function () { GMB.render(); } }))
        ]),
        h('h3', 'Announced controllers'),
        h('div.cc-list', caps.supportedCc.map(function (c) { return h('span.pill.mini', 'CC' + c); })),
        h('div.toolbar', [
          GMB.button('View raw SysEx bytes', function () { runSysEx('capabilities'); }, 'ghost'),
          GMB.button('Send change notification (block 8)', function () { runSysEx('notify'); }, 'ghost'),
          GMB.button('Regenerate device id', function () { GMB.toast('Device id regenerated (mock).', 'ok'); }, 'ghost')
        ])
      ]));
    }

    // ---- Integrated SysEx tester (section 18) -------------------------------
    var out = h('div#sysex-out.sysex-out', h('p.muted', 'Run a request to see the sent + received bytes and decoded fields.'));
    host.appendChild(h('div.card', [
      h('h2', 'SysEx tester'),
      h('div.toolbar.wrap', [
        GMB.button('Request identity', function () { runSysEx('identity'); }),
        GMB.button('Request descriptor', function () { runSysEx('descriptor'); }),
        GMB.button('Request capabilities', function () { runSysEx('capabilities'); }),
        GMB.button('Request string config', function () { runSysEx('stringConfig'); }),
        GMB.button('Notify change', function () { runSysEx('notify'); }),
        GMB.button('Full discovery', fullDiscovery, 'primary')
      ]),
      out
    ]));
  }

  function capItem(label, value) {
    return h('div.stat', [h('div.stat-label', label), h('div.stat-value', String(value))]);
  }

  function publish() {
    // Only a validated profile may be announced (SysEx spec 15).
    var problems = GMB.validateProfile(GMB.state.profile);
    if (problems.length) { GMB.toast('Fix ' + problems.length + ' issue(s) before publishing.', 'warn'); return; }
    GMB.saveProfile().then(function () {
      GMB.api.sysexRequest('notify').then(function () {
        GMB.toast('Capabilities published (block 8 notification sent).', 'ok');
      }).catch(function (e) { GMB.toast('Publish failed: ' + sysexErr(e), 'error'); });
    });
  }

  function testComms() {
    GMB.api.sysexRequest('identity').then(function (res) {
      lastDetection = res.valid ? ('OK — ' + new Date().toLocaleTimeString()) : 'Invalid response';
      var el = document.getElementById('last-detection');
      if (el) el.textContent = lastDetection;
      GMB.toast('Communication test: ' + (res.valid ? 'success' : 'failed'), res.valid ? 'ok' : 'error');
    }).catch(function (e) { GMB.toast('Communication test failed: ' + sysexErr(e), 'error'); });
  }

  function runSysEx(kind) {
    GMB.api.sysexRequest(kind)
      .then(function (res) { showResult(kind, res, true); })
      .catch(function (e) { GMB.toast('SysEx ' + kind + ' failed: ' + sysexErr(e), 'error'); });
  }

  function fullDiscovery() {
    var order = ['identity', 'capabilities', 'stringConfig', 'descriptor'];
    var out = document.getElementById('sysex-out');
    out.innerHTML = '';
    var chain = Promise.resolve();
    order.forEach(function (kind) {
      chain = chain.then(function () {
        return GMB.api.sysexRequest(kind).then(function (res) { showResult(kind, res, false); });
      });
    });
    chain.then(function () { GMB.toast('Full discovery complete.', 'ok'); })
      .catch(function (e) { GMB.toast('Discovery failed: ' + sysexErr(e), 'error'); });
  }

  function sysexErr(e) { return (e && e.body && e.body.error) || (e && e.message) || 'error'; }

  function showResult(kind, res, replace) {
    var out = document.getElementById('sysex-out');
    if (replace) out.innerHTML = '';
    var decoded = h('table.mini-table', [h('tbody', Object.keys(res.decoded).map(function (k) {
      return h('tr', [h('td', k), h('td', String(res.decoded[k]))]);
    }))]);
    out.appendChild(h('div.sysex-block', [
      h('div.sysex-title', [h('strong', kind), h('span.pill.mini.' + (res.valid ? 'ok' : 'error'), res.valid ? 'valid 7-bit' : 'invalid'),
        h('span.muted', res.length + ' bytes · ' + res.durationMs + ' ms')]),
      h('div.bytes-row', [h('span.byte-label', 'sent'), h('code', res.request)]),
      h('div.bytes-row', [h('span.byte-label', 'recv'), h('code', res.response)]),
      h('div.table-wrap', decoded)
    ]));
  }

  GMB.views.sysex = { render: render };
})(window);
