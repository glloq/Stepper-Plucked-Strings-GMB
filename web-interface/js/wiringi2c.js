/*
 * wiringi2c.js — "I²C & PCA" sub-tab of the Wiring & GPIO page.
 *
 * The live counterpart of hardware/I2C_PCA9685.md: per I²C bus it shows the
 * SDA/SCL pins, every PCA9685 with its address and the A2·A1·A0 solder-jumper
 * setting, and manages the PULL-UP BUDGET — the rule multi-board builds get
 * wrong: every breakout's on-board pull-ups sit in PARALLEL, so the page
 * computes the bus's EQUIVALENT resistance from the per-board declarations
 * (profile.hardware.pcaPullups) plus an optional external pair, and grades it
 * against the target window (one equivalent 2.2–4.7 kΩ per line).
 *
 * A board's pull-ups start as UNKNOWN (audit P1): no entry exists until the
 * builder has physically checked the breakout and declared what is fitted —
 * rendering the page never writes anything into the profile, and an unverified
 * bus reports "cannot be verified" instead of inventing stock 10 kΩ resistors.
 */
(function (global) {
  'use strict';
  var GMB = global.GMB, h = GMB.h;

  function hex2(n) { return '0x' + n.toString(16).toUpperCase(); }
  function jumpers(board) {
    return 'A2=' + ((board >> 2) & 1) + ' A1=' + ((board >> 1) & 1) + ' A0=' + (board & 1);
  }

  // READ-ONLY lookup: the declared pull-up note for (bus, board), or null when
  // the board has not been checked yet (unknown). Never creates entries.
  function noteFor(hw, bus, board) {
    for (var i = 0; i < hw.pcaPullups.length; i++) {
      var n = hw.pcaPullups[i];
      if (n.bus === bus && n.board === board) return n;
    }
    return null;
  }
  // Declare (or clear, ohm === null) a board's pull-up state. Only ever called
  // from a user interaction — a render must not mutate the profile.
  function setNote(hw, bus, board, ohm) {
    var kept = hw.pcaPullups.filter(function (n) { return !(n.bus === bus && n.board === board); });
    hw.pcaPullups.length = 0;
    kept.forEach(function (n) { hw.pcaPullups.push(n); });
    if (ohm !== null) hw.pcaPullups.push({ bus: bus, board: board, ohm: ohm });
    GMB.markDirty();
  }

  // Equivalent per line over the DECLARED boards + the external pair. Any
  // undeclared board makes the equivalent unverifiable.
  function equivalent(hw, boards, bus) {
    var inv = 0, unknown = 0, declared = 0;
    boards.forEach(function (b) {
      var n = noteFor(hw, bus, b.board);
      if (!n) { unknown++; return; }
      declared++;
      if (n.ohm > 0) inv += 1 / n.ohm;
    });
    var ext = bus === 1 ? hw.extPullupOhm1 : hw.extPullupOhm0;
    if (ext > 0) inv += 1 / ext;
    return { unknown: unknown, declared: declared,
             ohm: inv > 0 ? 1 / inv : Infinity, ext: ext };
  }

  function fmtK(ohm) {
    if (!isFinite(ohm)) return 'none';
    return (Math.round(ohm / 100) / 10) + ' kΩ';
  }

  // Grade the equivalent against the practical window: the I²C spec floors the
  // sink current at ~3 mA (≈1.1 kΩ at 3.3 V) and long harnesses need ≲10 kΩ.
  function grade(ohm) {
    if (!isFinite(ohm)) return { cls: 'error', txt: 'no pull-up on the bus — the lines float' };
    if (ohm < 1500) return { cls: 'error', txt: 'too strong — below ~1.5 kΩ the drivers cannot sink the bus; disable some boards’ pull-ups' };
    if (ohm <= 5600) return { cls: 'ok', txt: 'in the target window (2.2–4.7 kΩ ideal)' };
    if (ohm <= 12000) return { cls: 'warn', txt: 'weak — edges get slow on a long harness; add/keep one 2.2–4.7 kΩ pair' };
    return { cls: 'error', txt: 'far too weak — fit one 2.2–4.7 kΩ pair on the bus' };
  }

  // Three-state declaration control for one board: Unknown (no entry) / None
  // (removed, ohm 0) / a value in kΩ (presets + custom).
  var PRESETS = [2200, 4700, 10000];
  function pullupControl(hw, bus, board) {
    var note = noteFor(hw, bus, board);
    var isCustom = note && note.ohm > 0 && PRESETS.indexOf(note.ohm) < 0;
    var sel = h('select');
    function opt(value, label, selected) {
      sel.appendChild(h('option', { value: value, selected: !!selected }, label));
    }
    opt('unknown', 'Unknown — not checked', !note);
    opt('none', 'None (removed / absent)', note && note.ohm === 0);
    PRESETS.forEach(function (o) {
      opt(String(o), (o / 1000) + ' kΩ', note && note.ohm === o);
    });
    opt('custom', 'Custom…', isCustom);
    sel.addEventListener('change', function () {
      var v = sel.value;
      if (v === 'unknown') setNote(hw, bus, board, null);
      else if (v === 'none') setNote(hw, bus, board, 0);
      else if (v === 'custom') setNote(hw, bus, board, note && note.ohm > 0 ? note.ohm : 10000);
      else setNote(hw, bus, board, Number(v));
      GMB.render();
    });
    var kids = [sel];
    if (isCustom) {
      var proxy = { v: note.ohm / 1000 };
      kids.push(GMB.input(proxy, 'v', { type: 'number', min: 0.1, max: 100, step: 0.1,
        coerce: Number,
        onChange: function (v) { if (v > 0) setNote(hw, bus, board, Math.round(v * 1000)); GMB.render(); } }));
      kids.push(h('span.muted', 'kΩ'));
    }
    return h('div.row', kids);
  }

  function busCard(p, hw, m, bus) {
    var boards = m.boards.filter(function (b) { return b.bus === bus; });
    var sda = bus === 1 ? m.sda2 : m.sda, scl = bus === 1 ? m.scl2 : m.scl;
    var sdaName = bus === 1 ? 'SDA2' : 'SDA', sclName = bus === 1 ? 'SCL2' : 'SCL';
    var gp = function (g) { return g >= 0 ? 'GPIO' + g : 'unassigned'; };

    var kids = [h('div.card-head', [h('h2', 'I²C bus ' + bus),
      h('span.muted', boards.length + ' board(s) · ' + sdaName + ' = ' + gp(sda) + ' · ' + sclName + ' = ' + gp(scl))])];
    if (sda < 0 || scl < 0)
      kids.push(h('div.pill.error', sdaName + '/' + sclName + ' unassigned — set them in the GPIO pins sub-tab.'));

    // Boards: address, jumpers, servos, on-board pull-up declaration.
    var rows = boards.map(function (b) {
      var chans = m.byBoard[b.key] || [], count = 0;
      for (var c = 0; c < 16; c++) count += (chans[c] ? chans[c].length : 0);
      var note = noteFor(hw, bus, b.board);
      return h('tr', [
        h('td', 'PCA #' + b.board),
        h('td', hex2(0x40 + b.board)),
        h('td', jumpers(b.board)),
        h('td', String(count)),
        h('td', pullupControl(hw, bus, b.board)),
        h('td.muted', !note ? '⚠ check the breakout' : (note.ohm === 0 ? 'removed / absent' : ''))
      ]);
    });
    kids.push(h('table.cap-table', [
      h('thead', h('tr', [h('th', 'Board'), h('th', 'Address'), h('th', 'Jumpers'),
        h('th', 'Servos'), h('th', 'On-board SDA/SCL pull-ups'), h('th', '')])),
      h('tbody', rows)
    ]));
    kids.push(h('p.muted', 'Set each board’s A0–A2 solder jumpers BEFORE mounting it. Then CHECK each ' +
      'breakout (schematic or multimeter) and declare what is really fitted — most ship 10 kΩ on SDA/SCL, ' +
      'some 4.7 kΩ, and a desoldered board is “None”. Nothing is assumed until you declare it.'));

    // External pair + the equivalent.
    var extKey = bus === 1 ? 'extPullupOhm1' : 'extPullupOhm0';
    var extProxy = { v: hw[extKey] > 0 ? hw[extKey] / 1000 : 0 };
    kids.push(h('div.grid', [
      GMB.field('External pull-up pair (kΩ, 0 = none)',
        GMB.input(extProxy, 'v', { type: 'number', min: 0, max: 100, step: 0.1, coerce: Number,
          onChange: function (v) { hw[extKey] = v > 0 ? Math.round(v * 1000) : 0; GMB.render(); } }),
        'one resistor per line to 3.3 V, at the ESP32 end')
    ]));
    var eq = equivalent(hw, boards, bus);
    if (eq.unknown > 0) {
      kids.push(h('div.ps-row', [h('span.pill.mini.warn', 'unverified'),
        h('span', 'Equivalent per line: cannot be verified'),
        h('span.muted', ' — ' + eq.unknown + ' board(s) not checked yet' +
          (eq.declared || eq.ext ? ' (declared so far: ' + fmtK(eq.ohm) + ')' : ''))]));
    } else {
      var g = grade(eq.ohm);
      kids.push(h('div.ps-row', [h('span.pill.mini.' + g.cls, fmtK(eq.ohm)),
        h('span', 'Equivalent per line (all pull-ups in parallel)'),
        h('span.muted', ' — ' + g.txt)]));
    }
    kids.push(h('p.muted', 'ONE correctly sized equivalent per bus line — never “a pair per PCA”. ' +
      'Rule and worked examples: hardware/I2C_PCA9685.md §3.'));
    return h('div.card', kids);
  }

  function render(host) {
    var p = GMB.state.profile;
    var hw = GMB.ensureHardware(p);
    var m = GMB.wiringModel(p);

    host.appendChild(h('div.card', [
      h('div.card-head', [h('h2', 'I²C & PCA9685'),
        h('span.muted', 'addresses, jumpers and the pull-up budget per bus')]),
      h('p.muted', 'A physical board is identified by (bus, address): each bus has its own 0x40–0x47 range — ' +
        'up to 8 boards per bus, 16 total. Two boards on the SAME bus must never share an address. ' +
        'Reference: hardware/I2C_PCA9685.md.')
    ]));

    if (!m.usePca) {
      host.appendChild(h('div.card', [h('div.pill.warn', 'No PCA9685 in this configuration.'),
        h('p.muted', 'Every servo is on a direct GPIO — there is no I²C bus to manage.')]));
      return;
    }

    host.appendChild(busCard(p, hw, m, 0));
    if (m.useBus1) host.appendChild(busCard(p, hw, m, 1));

    // Declarations for boards that no longer exist are harmless but worth surfacing.
    var live = {};
    m.boards.forEach(function (b) { live[b.bus + ':' + b.board] = 1; });
    var orphans = hw.pcaPullups.filter(function (n) { return !live[n.bus + ':' + n.board]; });
    if (orphans.length) {
      host.appendChild(h('div.card', [
        h('p.muted', orphans.length + ' pull-up declaration(s) refer to boards no longer in the configuration.'),
        h('div.row', [GMB.button('Clean up', function () {
          var keep = hw.pcaPullups.filter(function (n) { return live[n.bus + ':' + n.board]; });
          hw.pcaPullups.length = 0;
          keep.forEach(function (n) { hw.pcaPullups.push(n); });
          GMB.markDirty(); GMB.render();
        }, 'ghost')])
      ]));
    }
  }

  GMB.views.wiringI2c = { render: render };
})(window);
