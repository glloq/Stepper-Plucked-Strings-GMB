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

    function push(ev) {
      if (t0 === null) t0 = ev.t || Date.now();
      var rel = ((ev.t || Date.now()) - t0);
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
        h('td', ev.interpretation || '')
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
