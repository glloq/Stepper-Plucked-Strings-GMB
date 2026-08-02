/*
 * midimonitor.js — real-time MIDI monitor component (selection spec section 15).
 *
 * Reusable widget: GMB.midiMonitor.mount(container) subscribes to the /ws/midi
 * stream and renders a Time / Channel / Message / Value / Interpretation table
 * with a clear-log button. Used on the MIDI page and reused elsewhere.
 */
(function (global) {
  'use strict';
  var GMB = global.GMB, h = GMB.h;

  var MAX_ROWS = 200;

  function mount(container) {
    var rows = [];
    var conn = null;
    var tbody = h('tbody');
    var t0 = null;

    var wrap = h('div.monitor', [
      h('div.monitor-head', [
        h('span.pill.mini.ok', 'listening'),
        h('span.muted', 'CC / Note events with tablature interpretation'),
        h('span.spacer'),
        GMB.button('Clear log', function () { rows = []; tbody.innerHTML = ''; t0 = null; }, 'ghost')
      ]),
      h('div.table-wrap.monitor-scroll', h('table.monitor-table', [
        h('thead', h('tr', [h('th', 'Time'), h('th', 'Ch'), h('th', 'Message'), h('th', 'Value'), h('th', 'Interpretation')])),
        tbody
      ]))
    ]);
    container.appendChild(wrap);

    conn = GMB.api.connectMidi(function (ev) { push(ev); });

    // Normalise the firmware DTO ({timestampUs, type:"controlChange", data1,
    // data2}) and the mock DTO ({t, type:"cc", cc, value, note}) to one shape.
    function normalize(ev) {
      var type = ev.type === 'controlChange' ? 'cc' : ev.type;
      var isCc = type === 'cc';
      var d1 = ev.data1 !== undefined ? ev.data1 : (isCc ? ev.cc : ev.note);
      var d2 = ev.data2 !== undefined ? ev.data2 : ev.value;
      var ts = ev.timestampUs !== undefined ? ev.timestampUs / 1000 : (ev.t || Date.now());
      return {
        type: type, channel: ev.channel,
        cc: isCc ? d1 : undefined,
        note: isCc ? undefined : (ev.note !== undefined ? ev.note : d1),
        value: d2, ts: ts, interpretation: ev.interpretation
      };
    }

    function ccName(cc) {
      if (cc === 7) return 'Volume';
      if (cc === 11) return 'Expression';
      if (cc === 64) return 'Sustain';
      if (cc === 120) return 'All Sound Off';
      if (cc === 123) return 'All Notes Off';
      return 'Controller ' + cc;
    }

    function interpret(n) {
      if (n.interpretation) return n.interpretation;
      if (n.type === 'cc') return ccName(n.cc);
      if (n.type === 'noteOn' || n.type === 'noteOff')
        return GMB.noteName ? GMB.noteName(n.note) : ('note ' + n.note);
      return '';
    }

    function push(raw) {
      var ev = normalize(raw);
      if (t0 === null) t0 = ev.ts;
      var rel = Math.round(ev.ts - t0);
      var msg = ev.type === 'cc' ? ('CC' + ev.cc)
        : ev.type === 'noteOn' ? ('Note On ' + ev.note)
        : ev.type === 'noteOff' ? ('Note Off ' + ev.note)
        : ev.type;
      var cls = ev.type === 'noteOn' ? 'row-note' : (ev.type === 'cc' ? 'row-cc' : '');
      var tr = h('tr.' + (cls || 'row'), [
        h('td', rel + ' ms'),
        h('td', String(ev.channel)),
        h('td', msg),
        h('td', String(ev.value !== undefined ? ev.value : '')),
        h('td', interpret(ev))
      ]);
      tbody.insertBefore(tr, tbody.firstChild);
      rows.push(ev);
      while (tbody.children.length > MAX_ROWS) tbody.removeChild(tbody.lastChild);
      GMB.updateMockBadge();
    }

    return {
      el: wrap,
      push: push,
      close: function () { if (conn) conn.close(); }
    };
  }

  GMB.midiMonitor = { mount: mount };
})(window);
