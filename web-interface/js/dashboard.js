/*
 * dashboard.js — main dashboard (spec section 19).
 *
 * Shows the general state, Wi-Fi, MIDI source, active profile, strings-ready
 * count, notes playing, active faults, temperatures/voltages and a big STOP
 * (panic) button; plus a per-string table (state, note, fret, motor position,
 * target, distance, HOME/LIMIT, finger, plectrum, last fault). Live values
 * come from the /ws/status WebSocket (mock pump when standalone).
 */
(function (global) {
  'use strict';
  var GMB = global.GMB, h = GMB.h;

  var conn = null;

  function render(host) {
    if (conn) { conn.close(); conn = null; }

    var summary = h('div.grid.dash-summary');
    var faultsBox = h('div#dash-faults');
    var envBox = h('div.grid.dash-env');
    var table = h('div.table-wrap', h('table.string-table', [buildHead(), h('tbody#dash-tbody')]));

    host.appendChild(h('div.card.panic-card', [
      h('div', [h('h2', 'Emergency stop'),
        h('p.muted', 'Disables drivers, neutralises servos (PCA9685 /OE), flushes the MIDI queue.')]),
      h('div.panic-actions', [
        h('button.btn.danger.panic-big', { onclick: GMB.doPanic }, 'STOP'),
        h('button.btn', {
          title: 'Recover from a panic / E-stop and re-home (refused while E-stop or a LIMIT is active)',
          onclick: function () {
            GMB.api.resetSystem().then(function (res) {
              if (res && res.ok === false) {
                GMB.toast('Reset refused: ' + (res.error || 'E-stop/LIMIT active or invalid config') + '.', 'warn');
              } else {
                GMB.toast('Reset accepted — re-homing.', 'ok');
              }
            }).catch(function (e) { GMB.toast('Reset failed: ' + (e && e.message || e), 'error'); });
          }
        }, 'Reset & re-home')
      ])
    ]));
    host.appendChild(h('div.card', [h('h2', 'General state'), summary]));
    host.appendChild(h('div.card', [h('h2', 'Active faults'), faultsBox]));
    host.appendChild(h('div.card', [h('h2', 'Power & temperature'), envBox]));
    host.appendChild(h('div.card', [h('div.card-head', [h('h2', 'Strings'),
      h('span.muted', 'live per-string state')]), table]));

    update(sampleFromProfile());
    conn = GMB.api.connectStatus(function (st) { update(st); });
  }

  function sampleFromProfile() {
    // First paint before the socket delivers anything.
    var p = GMB.state.profile;
    return {
      state: '—', wifi: { mode: p.network.mode, ssid: p.network.apSsid, ip: '—', rssi: 0, connected: false },
      midiSource: '—', activeProfile: p.instrument.name,
      stringsReady: 0, stringsTotal: p.instrument.stringCount, notesPlaying: 0,
      faults: [], temperatures: [], voltages: [],
      capabilitiesRevision: p.capabilitiesRevision,
      strings: p.strings.map(function (s, i) {
        return { index: i, state: '—', note: null, fret: null, positionMm: 0, targetMm: 0,
          distanceMm: 0, home: false, limit: false, finger: '—', plectrum: '—', lastFault: 'none', openNote: s.openNote };
      })
    };
  }

  function buildHead() {
    var cols = ['#', 'Open', 'State', 'Note', 'Fret', 'Pos (mm)', 'Target', 'Dist',
      'HOME', 'LIMIT', 'Finger', 'Plectrum', 'Last fault'];
    return h('thead', h('tr', cols.map(function (c) { return h('th', c); })));
  }

  function update(st) {
    var sum = document.querySelector('.dash-summary');
    if (!sum) return;
    sum.innerHTML = '';
    var items = [
      ['State', stateBadge(st.state)],
      ['Wi-Fi', st.wifi.mode === 'station' ? ('Client — ' + st.wifi.ssid) : ('Access point — ' + st.wifi.ssid)],
      ['IP address', st.wifi.ip + (st.wifi.rssi ? '  (' + st.wifi.rssi + ' dBm)' : '')],
      ['MIDI source', st.midiSource],
      ['Active profile', st.activeProfile],
      ['Strings ready', st.stringsReady + ' / ' + st.stringsTotal],
      ['Notes playing', String(st.notesPlaying)],
      ['Capabilities rev.', String(st.capabilitiesRevision)]
    ];
    items.forEach(function (it) {
      sum.appendChild(h('div.stat', [h('div.stat-label', it[0]),
        typeof it[1] === 'string' ? h('div.stat-value', it[1]) : h('div.stat-value', it[1])]));
    });

    var fb = document.getElementById('dash-faults');
    fb.innerHTML = '';
    if (!st.faults || !st.faults.length) {
      fb.appendChild(h('div.pill.ok', 'No active faults'));
    } else {
      st.faults.forEach(function (f) {
        // The firmware sends {source, message, atMs}; tolerate a bare string too.
        var text = (f && typeof f === 'object')
          ? ((f.source ? f.source + ': ' : '') + (f.message || ''))
          : String(f);
        fb.appendChild(h('div.pill.error', text));
      });
    }

    var env = document.querySelector('.dash-env');
    env.innerHTML = '';
    (st.temperatures || []).forEach(function (t) {
      env.appendChild(h('div.stat', [h('div.stat-label', t.name), h('div.stat-value', t.c + ' °C')]));
    });
    (st.voltages || []).forEach(function (v) {
      env.appendChild(h('div.stat', [h('div.stat-label', v.name), h('div.stat-value', v.v + ' V')]));
    });
    if (!env.children.length) env.appendChild(h('div.muted', 'No sensors reported.'));

    var tb = document.getElementById('dash-tbody');
    tb.innerHTML = '';
    (st.strings || []).forEach(function (s) {
      tb.appendChild(h('tr', [
        h('td', String(s.index + 1)),
        h('td', GMB.noteName(s.openNote)),
        h('td', h('span.pill.mini.' + stateClass(s.state), s.state)),
        h('td', s.note === null ? '—' : GMB.noteName(s.note)),
        h('td', s.fret === null ? '—' : String(s.fret)),
        h('td', fmt(s.positionMm)),
        h('td', fmt(s.targetMm)),
        h('td', fmt(s.distanceMm)),
        h('td', dot(s.home)),
        h('td', dot(s.limit)),
        h('td', s.finger),
        h('td', s.plectrum),
        h('td', s.lastFault === 'none' ? h('span.muted', 'none') : h('span.pill.mini.error', s.lastFault))
      ]));
    });
  }

  function fmt(v) { return (v === null || v === undefined) ? '—' : Number(v).toFixed(1); }
  function dot(on) { return h('span.leddot' + (on ? '.on' : '')); }
  function stateBadge(s) { return h('span.pill.' + stateClass(s), s); }
  function stateClass(s) {
    var u = String(s).toUpperCase();
    if (u === 'READY' || u === 'IDLE') return 'ok';
    if (u === 'READYDEGRADED') return 'warn';  // playing, but some axes disabled
    if (u === 'FAULT' || u === 'DISABLED' || u === 'PANIC' || u === 'EMERGENCYSTOP')
      return 'error';
    if (s === '—') return 'muted';
    return 'warn';
  }

  GMB.views.dashboard = { render: render };
})(window);
