/*
 * fretboard.js — the "Instrument" page: a playable, live view of the machine.
 *
 * This is the stepper build's answer to the servo build's fretboard. There, every
 * fret has its own servo and the page is a map of which finger to press. Here ONE
 * carriage per string slides to the fret, so the interesting thing to show is not
 * "which servo" but WHERE EACH CARRIAGE IS — live, in millimetres, against the
 * fretboard geometry the profile declares.
 *
 * What it draws:
 *   • a fretboard laid out by the real equal-tempered geometry (GMB.fretAbsoluteMm),
 *     so a calibrated fret sits where it was MEASURED, not where theory puts it;
 *   • one lane per string with a live carriage marker fed by /ws/status, plus a
 *     ghost marker at the commanded target while it is still travelling;
 *   • per-string state (idle / moving / pressed / sounding / FAULTED) and the note
 *     currently sounding.
 *
 * What it does:
 *   • click a fret to PLAY it — that goes through /api/test/note, the same path a
 *     MIDI note takes, so it exercises the whole chain (allocate → move → press →
 *     pluck → damp) rather than poking one actuator;
 *   • strum / chord buttons that play several strings together;
 *   • a jog control per string for bring-up and fret calibration.
 *
 * Everything is read from GMB.state.profile + the live status; nothing here caches
 * mechanical state of its own, so it can never disagree with the device.
 */
(function (global) {
  'use strict';
  var GMB = global.GMB, h = GMB.h;
  var SVGNS = 'http://www.w3.org/2000/svg';

  // ---- geometry -------------------------------------------------------------
  var NUT_X = 96;        // x of fret 0 (the nut)
  var RIGHT_X = 860;     // x of the highest fret of the longest string
  var LANE_H = 46;       // vertical pitch between strings
  var TOP = 58;          // headroom for the fret-number row
  var BOT = 34;

  var status = null;     // last /ws/status frame
  var sock = null;       // live status socket
  var selected = 0;      // string the jog controls act on
  var lastPlayed = {};   // stringIndex -> fret, for the click highlight

  // ---- tiny SVG builder -----------------------------------------------------
  function svg(tag, attrs, kids) {
    var el = document.createElementNS(SVGNS, tag);
    if (attrs) Object.keys(attrs).forEach(function (k) {
      var v = attrs[k];
      if (v === null || v === undefined || v === false) return;
      if (k === 'text') el.textContent = v;
      else if (k === 'onclick') el.addEventListener('click', v);
      else el.setAttribute(k, v);
    });
    appendKids(el, kids);
    return el;
  }
  function appendKids(el, kids) {
    if (kids === null || kids === undefined) return;
    if (Array.isArray(kids)) { kids.forEach(function (k) { appendKids(el, k); }); return; }
    if (kids.nodeType) { el.appendChild(kids); return; }
    el.appendChild(document.createTextNode(String(kids)));
  }

  // ---- profile helpers ------------------------------------------------------
  function strings() { return (GMB.state.profile.strings || []); }
  function enabledStrings() {
    return strings().filter(function (s) { return s.enabled !== false; });
  }
  // The enabled strings WITH their original index. Filtering compacts the array,
  // so the position in the filtered list is not the physical axis: with string 2
  // disabled, filtered[1] is axis 2, and using 1 aims a whole chord at the wrong
  // carriages. Anything that needs an axis number must come through here.
  function enabledStringIndexes() {
    var out = [];
    strings().forEach(function (s, i) { if (s.enabled !== false) out.push(i); });
    return out;
  }
  // The travel every lane is scaled against: the furthest any carriage must reach,
  // so all the strings share one horizontal scale and a chord reads as a shape.
  function spanMm() {
    var max = 0;
    strings().forEach(function (s) {
      if (s.enabled === false) return;
      var v = GMB.fretAbsoluteMm(s, s.maxFret || 0);
      if (v > max) max = v;
    });
    return max > 0 ? max : 1;
  }
  function xForMm(mm) {
    return NUT_X + (RIGHT_X - NUT_X) * (mm / spanMm());
  }
  function xForFret(s, fret) { return xForMm(GMB.fretAbsoluteMm(s, fret)); }
  function noteAt(s, fret) {
    var p = GMB.state.profile;
    return s.openNote + fret + (p.instrument.capo || 0) +
           (p.instrument.transpose || 0) + ((p.midi && p.midi.transpose) || 0);
  }

  // Live per-string status, or null when the device has not reported yet.
  function stringStatus(i) {
    if (!status || !status.strings) return null;
    return status.strings[i] || null;
  }
  function isFaulted(i) {
    var st = stringStatus(i);
    return !!st && (st.state === 'FAULT' || st.state === 'FAULTED');
  }
  function armed() {
    if (!status) return false;
    var s = String(status.state || '').toLowerCase();
    return s === 'ready' || s === 'readydegraded';
  }

  // ---- playing --------------------------------------------------------------
  // A fret is played as a real MIDI note, not as a servo poke: /api/test/note runs
  // the whole chain the instrument uses in performance, so what you hear here is
  // what a controller would get. The firmware refuses it unless armed.
  function play(strIdx, fret, velocity, durationMs) {
    var s = strings()[strIdx];
    if (!s || s.enabled === false) return;
    if (!armed()) {
      GMB.toast('The instrument is not ready — home it first (it must reach Ready to play).', 'warn');
      return;
    }
    if (isFaulted(strIdx)) {
      GMB.toast('String ' + (strIdx + 1) + ' is faulted and out of service.', 'warn');
      return;
    }
    var p = GMB.state.profile;
    lastPlayed[strIdx] = fret;
    GMB.api.testNote({
      channel: (p.midi && p.midi.globalChannel) || 0,
      note: noteAt(s, fret),
      velocity: velocity || 100,
      durationMs: durationMs || 600
    }).catch(function () {
      GMB.toast('The device refused the note (not armed, or the string is busy).', 'warn');
    });
    paint();
  }

  // Play several (string, fret) pairs together. The firmware's chord window groups
  // them; a small stagger here only keeps the requests from racing in the queue.
  function playChord(picks, velocity) {
    picks.forEach(function (pk, n) {
      setTimeout(function () { play(pk.str, pk.fret, velocity); }, n * 12);
    });
  }

  // Lowest playable fret on each enabled string whose pitch class is in the chord.
  function chordPicks(pcs) {
    var picks = [];
    strings().forEach(function (s, i) {
      if (s.enabled === false || isFaulted(i)) return;
      for (var f = 0; f <= (s.maxFret || 0); f++) {
        if (pcs.indexOf(((s.openNote + f) % 12 + 12) % 12) >= 0) {
          picks.push({ str: i, fret: f });
          return;
        }
      }
    });
    return picks;
  }

  // ---- diagram --------------------------------------------------------------
  function buildDiagram() {
    var ss = strings();
    var height = TOP + ss.length * LANE_H + BOT;
    // NB: the class goes in the attribute map, never in the tag name —
    // createElementNS() takes the tag LITERALLY, so 'svg.fb-svg' would create an
    // element actually named "svg.fb-svg" that no browser draws.
    var root = svg('svg', {
      class: 'fb-svg',
      viewBox: '0 0 ' + (RIGHT_X + 120) + ' ' + height,
      width: '100%', preserveAspectRatio: 'xMinYMin meet',
      role: 'group', 'aria-label': 'Instrument fretboard with live carriage positions'
    });

    // Fret wires + numbers, taken from the LONGEST string so the scale is shared.
    var ref = enabledStrings()[0] || ss[0];
    if (ref) {
      var maxFret = 0;
      ss.forEach(function (s) { if (s.enabled !== false && s.maxFret > maxFret) maxFret = s.maxFret; });
      for (var f = 0; f <= maxFret; f++) {
        var x = xForFret(ref, f);
        root.appendChild(svg('line', {
          class: f === 0 ? 'fb-nut' : 'fb-fret',
          x1: x, y1: TOP - 12, x2: x, y2: TOP + ss.length * LANE_H - LANE_H / 2 + 12
        }));
        // Number every fret while they are far apart, then every 5th (plus 12/24)
        // so the row stays readable as the frets crowd toward the bridge.
        var label = (f <= 5 || f % 5 === 0 || f === 12 || f === 24);
        if (label) root.appendChild(svg('text', {
          class: 'fb-fretnum', x: x, y: TOP - 22, 'text-anchor': 'middle', text: String(f)
        }));
      }
    }

    ss.forEach(function (s, i) {
      var y = TOP + i * LANE_H;
      var off = s.enabled === false;
      var faulted = isFaulted(i);
      var g = svg('g', { class: 'fb-lane' + (off ? ' off' : '') + (faulted ? ' faulted' : '') });

      // The string itself.
      g.appendChild(svg('line', {
        class: 'fb-string', x1: NUT_X - 40, y1: y, x2: RIGHT_X + 40, y2: y
      }));
      // Label: string number + open note, and the fault flag when it is out.
      g.appendChild(svg('text', {
        class: 'fb-label', x: 8, y: y + 4,
        text: (i + 1) + ' · ' + GMB.noteName(s.openNote)
      }));
      if (faulted) g.appendChild(svg('text', {
        class: 'fb-fault', x: 8, y: y + 18, text: 'FAULT'
      }));

      if (!off && !faulted) {
        // Clickable fret targets. The hit area spans to the NEXT fret so the high
        // frets stay reachable even when the wires are millimetres apart.
        for (var f = 0; f <= (s.maxFret || 0); f++) {
          (function (fret) {
            var xa = xForFret(s, fret);
            var xb = fret < s.maxFret ? xForFret(s, fret + 1) : xa + 14;
            var w = Math.max(10, xb - xa);
            g.appendChild(svg('rect', {
              class: 'fb-hit' + (lastPlayed[i] === fret ? ' played' : ''),
              x: xa, y: y - LANE_H / 2 + 4, width: w, height: LANE_H - 8,
              rx: 3, tabindex: 0,
              onclick: function () { play(i, fret); }
            }));
          })(f);
        }
      }

      // Live carriage: the commanded target as a ghost, the real position solid.
      var st = stringStatus(i);
      if (st && typeof st.positionMm === 'number') {
        if (typeof st.targetMm === 'number' && Math.abs(st.targetMm - st.positionMm) > 0.3) {
          g.appendChild(svg('circle', {
            class: 'fb-target', cx: xForMm(st.targetMm), cy: y, r: 7
          }));
        }
        g.appendChild(svg('circle', {
          class: 'fb-carriage' + (st.active ? ' sounding' : ''),
          cx: xForMm(st.positionMm), cy: y, r: 9
        }));
        g.appendChild(svg('text', {
          class: 'fb-pos', x: RIGHT_X + 52, y: y + 4,
          text: st.positionMm.toFixed(1) + ' mm'
        }));
      } else if (!off) {
        g.appendChild(svg('text', {
          class: 'fb-pos muted', x: RIGHT_X + 52, y: y + 4, text: '— mm'
        }));
      }
      root.appendChild(g);
    });
    return root;
  }

  // Repaint only the diagram, so a status frame does not rebuild the whole page
  // (and does not fight a jog input the user is typing into).
  function paint() {
    var host = document.querySelector('.fb-scroll');
    if (!host) return;
    host.innerHTML = '';
    host.appendChild(buildDiagram());
  }

  // ---- panels ---------------------------------------------------------------
  function statePill() {
    if (!status) return h('span.pill', 'connecting…');
    var s = String(status.state || '').toLowerCase();
    if (s === 'ready') return h('span.pill.ok', 'Ready');
    if (s === 'readydegraded') return h('span.pill.warn', 'Ready (degraded)');
    if (s === 'homing') return h('span.pill.warn', 'Homing…');
    if (s === 'reconfiguring') return h('span.pill.warn', 'Reconfiguring…');
    if (s === 'configsafe') return h('span.pill.warn', 'Config-safe — no valid profile');
    return h('span.pill.warn', status.state || 'boot');
  }

  // ---- operating dashboard --------------------------------------------------
  //
  // The state that decides whether the machine will do anything at all used to be
  // one pill in a heading, with the faults behind the Settings modal. That is the
  // wrong place for it: when the instrument will not play, "why" should be on the
  // page you are already looking at. Four readings — phase, axes ready, notes
  // sounding, transport — plus the faults themselves, in full.
  function dashboard() {
    if (!status) return h('div.dash', h('span.muted', 'Connecting to the device…'));
    var ss = strings();
    var total = ss.filter(function (s) { return s.enabled !== false; }).length;
    var ready = 0, faults = [], playing = 0;
    ss.forEach(function (s, i) {
      if (s.enabled === false) return;
      var st = stringStatus(i);
      if (!st) return;
      if (isFaulted(i)) {
        faults.push({ i: i, why: st.lastFault && st.lastFault !== 'none' ? st.lastFault : 'faulted' });
      } else if (st.home !== false) {
        ready++;
      }
      if (st.note !== null && st.note !== undefined) playing++;
    });
    // Prefer the device's own count when it reports one — it knows about axes the
    // profile draft has not caught up with.
    if (typeof status.stringsReady === 'number') ready = status.stringsReady;
    if (typeof status.notesPlaying === 'number') playing = status.notesPlaying;

    var s = String(status.state || '').toLowerCase();
    var phaseCls = (s === 'ready') ? 'ok'
      : (s === 'readydegraded' || s === 'homing' || s === 'reconfiguring') ? 'warn' : 'error';

    var tiles = [
      tile('State', status.state || 'boot', phaseCls),
      tile('Axes ready', ready + ' / ' + total, ready === total && total > 0 ? 'ok' : 'warn'),
      tile('Notes sounding', String(playing), null),
      tile('MIDI in', midiSourceLabel(status), status.midiSourcePolicy === 'disabled' ? 'warn' : null)
    ];
    var kids = [h('div.dash', tiles)];

    // Faults get their own block, listed rather than counted: "2 faults" tells you
    // nothing you can act on.
    var devFaults = (status.faults || []).slice();
    if (faults.length || devFaults.length) {
      kids.push(h('div.dash-faults', [
        h('div.card-head', [h('h3', 'Faults'),
          h('span.muted', 'the instrument keeps playing on the axes that are still good')]),
        h('ul.problem-list',
          faults.map(function (f) {
            return h('li', 'String ' + (f.i + 1) + ' — ' + f.why + ' (out of service until reset)');
          }).concat(devFaults.map(function (f) {
            return h('li', typeof f === 'string' ? f : (f.message || JSON.stringify(f)));
          }))),
        h('div.toolbar', [
          GMB.button('Clear faults & re-home', function () {
            GMB.api.resetSystem().then(function (r) {
              GMB.toast(r && r.ok === false
                ? ('Reset refused: ' + (r.error || 'E-stop still latched')) : 'Reset requested — homing.',
                r && r.ok === false ? 'warn' : 'ok');
            }).catch(function (e) { GMB.toast('Reset failed: ' + (e && e.message || e), 'error'); });
          }, 'primary'),
          GMB.button('Open diagnostics', function () { GMB.openSettings('diagnostics'); }, 'ghost')
        ])
      ]));
    } else if (s === 'configsafe') {
      // Boot-safe is not a fault, but it IS the reason nothing moves, and the
      // answer is a different page.
      kids.push(h('div.dash-faults', [
        h('p.warn-text', 'No valid profile: the device booted config-safe and will not arm. ' +
          'Build or load an instrument, then save & publish.'),
        h('div.toolbar', [
          GMB.button('Go to Setup', function () { GMB.navigate('wizard'); }, 'primary'),
          GMB.button('Load a saved profile', function () { GMB.openSettings('profiles'); }, 'ghost')
        ])
      ]));
    }
    return kids;
  }

  function tile(label, value, cls) {
    return h('div.dash-tile' + (cls ? '.' + cls : ''),
      [h('span.dash-label', label), h('span.dash-value', String(value))]);
  }

  function midiSourceLabel(st) {
    if (st.midiSourcePolicy === 'disabled') return 'off';
    var src = st.midiSource || 'wifiUdp';
    if (src === 'none') return 'no input';
    var name = { wifiUdp: 'Wi-Fi UDP', din: 'DIN', usb: 'USB' }[src] || src;
    // Name the second live input too, so a DIN cable plugged in beside the Wi-Fi
    // link is visible from the dashboard rather than only in Settings.
    var others = (st.midiTransports || []).filter(function (t) {
      return t.bound && t.name !== src;
    });
    if (others.length) name += ' +' + others.length;
    if (st.midiSourcePolicy === 'lockToFirst') name += st.midiSourceLocked ? ' · locked' : ' · unlocked';
    return name;
  }

  var CHORDS = [
    { name: 'C',  pcs: [0, 4, 7] },
    { name: 'G',  pcs: [7, 11, 2] },
    { name: 'D',  pcs: [2, 6, 9] },
    { name: 'Am', pcs: [9, 0, 4] },
    { name: 'Em', pcs: [4, 7, 11] },
    { name: 'F',  pcs: [5, 9, 0] }
  ];

  function playCard() {
    return h('div.card', [
      h('div.card-head', [h('h2', 'Play'), h('span.muted', 'click a fret, or strum a chord')]),
      h('div.row', [
        GMB.button('Strum open strings', function () {
          playChord(enabledStringIndexes()
            .filter(function (i) { return !isFaulted(i); })
            .map(function (i) { return { str: i, fret: 0 }; }), 96);
        }, 'primary'),
        GMB.button('All strings, fret 5', function () {
          var picks = [];
          strings().forEach(function (s, i) {
            if (s.enabled !== false && !isFaulted(i) && (s.maxFret || 0) >= 5)
              picks.push({ str: i, fret: 5 });
          });
          playChord(picks, 96);
        }, 'ghost')
      ]),
      h('div.row.chord-row', CHORDS.map(function (c) {
        return GMB.button(c.name, function () { playChord(chordPicks(c.pcs), 96); }, 'ghost');
      })),
      h('p.muted', 'Each click sends a real MIDI note through the whole chain (allocate → ' +
        'move the carriage → press → pluck → damp), so it tests what a controller would ' +
        'actually get. The device refuses notes unless it is Ready.')
    ]);
  }

  // Jog: the bring-up and fret-calibration tool. The firmware only accepts it on an
  // idle, homed, non-faulted axis and clamps it to the axis travel, so this cannot
  // fight a live note.
  function jogCard() {
    var ss = strings();
    if (!ss.length) return null;
    var opts = ss.map(function (s, i) {
      return h('option', { value: String(i), selected: i === selected ? 'selected' : null },
        'String ' + (i + 1) + ' · ' + GMB.noteName(s.openNote) +
        (s.enabled === false ? ' (disabled)' : ''));
    });
    function jog(mm) {
      if (!armed()) { GMB.toast('Jog needs the instrument Ready (homed and armed).', 'warn'); return; }
      GMB.api.jog({ axis: selected, deltaMm: mm }).catch(function () {
        GMB.toast('Jog refused — the axis is busy, faulted or not homed.', 'warn');
      });
    }
    return h('div.card', [
      h('div.card-head', [h('h2', 'Jog'), h('span.muted', 'bring-up & fret calibration')]),
      h('div.row', [
        h('select.jog-axis', {
          onchange: function (e) { selected = Number(e.target.value) || 0; }
        }, opts),
        GMB.button('−10', function () { jog(-10); }, 'ghost'),
        GMB.button('−1', function () { jog(-1); }, 'ghost'),
        GMB.button('−0.1', function () { jog(-0.1); }, 'ghost'),
        GMB.button('+0.1', function () { jog(0.1); }, 'ghost'),
        GMB.button('+1', function () { jog(1); }, 'ghost'),
        GMB.button('+10', function () { jog(10); }, 'ghost')
      ]),
      h('p.muted', 'Move the selected carriage by a known distance and MEASURE it on the ' +
        'machine: if the millimetres on screen are not the millimetres travelled, the ' +
        'transmission (steps/mm) is wrong and every fret will be off. One nudge is bounded ' +
        'to 25 mm and clamped to the axis travel.')
    ]);
  }

  function axesCard() {
    var ss = strings();
    if (!ss.length) return null;
    var rows = ss.map(function (s, i) {
      var st = stringStatus(i);
      var state = s.enabled === false ? 'disabled' : (st ? (st.state || '—') : '—');
      return h('tr' + (isFaulted(i) ? '.row-error' : ''), [
        h('td', h('strong', String(i + 1))),
        h('td', GMB.noteName(s.openNote)),
        h('td', '0–' + (s.maxFret || 0)),
        h('td', (s.fretOffsetMm || 0) + ' mm'),
        h('td', st && typeof st.positionMm === 'number' ? st.positionMm.toFixed(1) + ' mm' : '—'),
        h('td', st && st.note != null ? GMB.noteName(st.note) : '—'),
        h('td', state)
      ]);
    });
    return h('div.card', [
      h('div.card-head', [h('h2', 'Axes'), h('span.muted', 'live carriage state')]),
      h('table.cap-table', [
        h('thead', h('tr', [h('th', '#'), h('th', 'Open'), h('th', 'Frets'),
          h('th', 'Nut offset'), h('th', 'Position'), h('th', 'Sounding'), h('th', 'State')])),
        h('tbody', rows)
      ])
    ]);
  }

  // ---- view -----------------------------------------------------------------
  function teardown() {
    if (sock) { try { sock.close(); } catch (e) {} sock = null; }
  }

  function render(host) {
    var p = GMB.state.profile;
    var ss = strings();

    host.appendChild(h('div.card', [
      h('div.card-head', [h('h2', p.instrument.name || 'Instrument'),
        statePill()]),
      h('div#fb-dash', dashboard()),
      h('p.muted', 'The live instrument: one lane per string, with each carriage drawn where ' +
        'it actually is. A hollow marker is where a carriage has been TOLD to go while it is ' +
        'still travelling. Fret spacing follows the real geometry — a calibrated fret sits ' +
        'where it was measured, not where the theory puts it.')
    ]));

    if (!ss.length) {
      host.appendChild(h('div.card', [h('div.pill.warn', 'No strings configured yet.'),
        h('p.muted', 'Set the instrument up first — the fretboard is built from its axes.'),
        h('div.row', [GMB.button('Go to setup', function () { GMB.navigate('wizard'); }, 'primary')])]));
      return;
    }

    host.appendChild(h('div.card.fb-card', [h('div.fb-scroll', buildDiagram())]));
    host.appendChild(playCard());
    var jc = jogCard();
    if (jc) host.appendChild(jc);
    var ac = axesCard();
    if (ac) host.appendChild(ac);

    // Live status. teardown() closes it when the page is left, and render() is
    // called again on every navigation, so re-subscribing here is safe.
    teardown();
    sock = GMB.api.connectStatus(function (frame) {
      status = frame;
      paint();
      var pill = host.querySelector('.card-head .pill');
      if (pill && pill.parentNode) pill.parentNode.replaceChild(statePill(), pill);
      // The dashboard is the reason to look at this page when something is wrong,
      // so it follows the live frame rather than the last full render.
      var dash = document.getElementById('fb-dash');
      if (dash) { dash.innerHTML = ''; GMB.appendChildren(dash, dashboard()); }
    });
    GMB.api.getStatus().then(function (s) { status = s; paint(); }).catch(function () {});
  }

  GMB.views.fretboard = { render: render, teardown: teardown };
})(window);
