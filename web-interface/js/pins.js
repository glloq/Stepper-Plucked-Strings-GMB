/*
 * pins.js — GPIO pin assignment grid (spec section 11).
 *
 * Colour categories: Green=recommended / Yellow=caution (advanced only) /
 * Red=reserved (never selectable) / Grey=used. Per-signal candidate lists are
 * filtered by capability (STEP needs fast output, HOME needs input+interrupt,
 * SDA/SCL need I2C...). An "Assign automatically" button reproduces the
 * recommended profile; validation reports conflicts with an explanation and a
 * suggested replacement pin.
 */
(function (global) {
  'use strict';
  var GMB = global.GMB, h = GMB.h;

  var board = null;

  function render(host) {
    var p = GMB.state.profile;
    if (!board) {
      GMB.api.getBoard(p.board.profile).then(function (b) { board = b; render(host); });
      host.appendChild(h('div.card', 'Loading board profile…'));
      return;
    }

    // Controls.
    host.appendChild(h('div.card', [
      h('div.card-head', [h('h2', 'Pin assignment — ' + board.displayName),
        h('span.muted', GMB.isAdvanced() ? 'Advanced: manual assignment + caution pins' : 'Simplified: recommended pins only')]),
      h('div.toolbar', [
        h('label.inline', [GMB.input(p.board, 'automaticPinAssignment', { type: 'checkbox' }), h('span', 'Automatic pin assignment')]),
        h('label.inline', [GMB.input(p.board, 'reserveUsb', { type: 'checkbox', onChange: function () { validate(); } }),
          h('span', 'Reserve GPIO19/20 for future USB')]),
        h('span.spacer'),
        GMB.button('Assign automatically', autoAssign, 'primary'),
        GMB.button('Validate', validate)
      ])
    ]));

    // Legend + grid.
    host.appendChild(h('div.card', [
      h('h2', 'Board GPIO map'),
      h('div.legend', [
        legend('recommended', 'Recommended'),
        legend('caution', 'Caution (advanced)'),
        legend('reserved', 'Reserved / incompatible'),
        legend('used', 'Used')
      ]),
      h('div#pin-grid.pin-grid')
    ]));

    // Signal assignment list.
    host.appendChild(h('div.card', [
      h('h2', 'Signals'),
      h('p.muted', 'Each signal only offers GPIOs compatible with it. Reserved pins are never listed.'),
      h('div#signal-list.signal-list')
    ]));

    host.appendChild(h('div.card', [h('h2', 'Validation'), h('div#pin-errors', h('div.pill.ok', 'Not checked yet.'))]));

    drawGrid();
    drawSignals();
    validate();
  }

  function legend(cls, label) { return h('span.legend-item', [h('span.swatch.' + cls), h('span', label)]); }

  // Which GPIO each signal currently uses.
  function usedMap(exceptSignal) {
    var m = {};
    GMB.state.profile.pins.forEach(function (a) {
      if (a.signal !== exceptSignal && a.gpio >= 0) m[a.gpio] = a.signal;
    });
    return m;
  }

  function pinCategory(cap, used) {
    if (used[cap.gpio]) return 'used';
    if (cap.reserved || cap.preference === 'reserved') return 'reserved';
    if (cap.usb && GMB.state.profile.board.reserveUsb) return 'reserved';
    return cap.preference; // recommended | caution
  }

  function drawGrid() {
    var grid = document.getElementById('pin-grid');
    grid.innerHTML = '';
    var used = usedMap();
    board.pins.forEach(function (cap) {
      var cat = pinCategory(cap, used);
      var cell = h('div.pin-cell.' + cat, { title: (cap.note || '') + (used[cap.gpio] ? ('  [' + used[cap.gpio] + ']') : '') }, [
        h('span.pin-gpio', 'GPIO' + cap.gpio),
        h('span.pin-tag', used[cap.gpio] ? used[cap.gpio] : catLabel(cap, cat))
      ]);
      grid.appendChild(cell);
    });
  }

  function catLabel(cap, cat) {
    if (cat === 'reserved') { if (cap.usb) return 'USB'; if (cap.strapping) return 'strap'; if (cap.onboardPeripheral) return 'onboard'; return 'reserved'; }
    if (cat === 'caution') return 'caution';
    return 'free';
  }

  // Signals derived from the current string count + interfaces.
  function signalSpecs() {
    var p = GMB.state.profile;
    var n = p.instrument.stringCount;
    var specs = [];
    for (var i = 0; i < n; i++) {
      specs.push({ signal: 'STEP' + (i + 1), kind: 'step', label: 'STEP ' + (i + 1) });
      specs.push({ signal: 'DIR' + (i + 1), kind: 'dir', label: 'DIR ' + (i + 1) });
      specs.push({ signal: 'HOME' + (i + 1), kind: 'home', label: 'HOME ' + (i + 1) });
    }
    specs.push({ signal: 'SDA', kind: 'sda', label: 'I2C SDA' });
    specs.push({ signal: 'SCL', kind: 'scl', label: 'I2C SCL' });
    specs.push({ signal: 'ENABLE', kind: 'enable', label: 'Global ENABLE' });
    specs.push({ signal: 'SERVO_OE', kind: 'servoOe', label: 'PCA9685 /OE (safety)' });
    return specs;
  }

  function currentGpio(signal) {
    var a = GMB.state.profile.pins.filter(function (x) { return x.signal === signal; })[0];
    return a ? a.gpio : -1;
  }

  function setGpio(signal, kind, gpio) {
    var pins = GMB.state.profile.pins;
    var a = pins.filter(function (x) { return x.signal === signal; })[0];
    if (!a) { a = { signal: signal, kind: kind, gpio: -1 }; pins.push(a); }
    a.gpio = gpio;
    a.kind = kind;
    GMB.markDirty();
  }

  // Candidate GPIOs for a signal (spec 11.3): compatible, not
  // reserved, not used elsewhere. Caution pins only surface in advanced mode.
  function candidates(kind, signal) {
    var reserveUsb = GMB.state.profile.board.reserveUsb;
    var used = usedMap(signal);
    var wantKind = GMB.SIGNAL_KIND[kind] || 'generic';
    return board.pins.filter(function (cap) {
      if (used[cap.gpio]) return false;
      if (cap.reserved || cap.preference === 'reserved') return false;
      if (cap.usb && reserveUsb) return false;
      if (cap.preference === 'caution' && !GMB.isAdvanced()) return false;
      return GMB.pinSupports(cap, wantKind);
    });
  }

  function drawSignals() {
    var list = document.getElementById('signal-list');
    list.innerHTML = '';
    signalSpecs().forEach(function (spec) {
      var cur = currentGpio(spec.signal);
      var cands = candidates(spec.kind, spec.signal);
      var sel = h('select');
      sel.appendChild(h('option', { value: -1, selected: cur < 0 }, '— unassigned —'));
      // Keep the current pin visible even if it is a caution pin, so advanced
      // choices survive a mode switch.
      var seen = {};
      cands.forEach(function (cap) {
        seen[cap.gpio] = true;
        sel.appendChild(h('option', { value: cap.gpio, selected: cap.gpio === cur },
          'GPIO' + cap.gpio + (cap.preference === 'caution' ? ' (caution)' : '')));
      });
      if (cur >= 0 && !seen[cur]) {
        sel.appendChild(h('option', { value: cur, selected: true }, 'GPIO' + cur + ' (current)'));
      }
      sel.addEventListener('change', function () {
        setGpio(spec.signal, spec.kind, Number(sel.value));
        drawGrid(); drawSignals(); validate();
      });
      list.appendChild(h('div.signal-row', [
        h('span.signal-name', spec.label),
        h('span.signal-kind', spec.kind.toUpperCase()),
        sel
      ]));
    });
  }

  function autoAssign() {
    var p = GMB.state.profile;
    GMB.api.autoPins({
      stringCount: p.instrument.stringCount, useI2cServos: true,
      globalEnable: true, servoSafetyOe: true, reserveUsb: p.board.reserveUsb
    }).then(function (res) {
      if (res.errors && res.errors.length) {
        GMB.toast('Auto-assign could not place every signal.', 'warn');
      } else {
        GMB.toast('Pins assigned automatically.', 'ok');
      }
      p.pins = res.pins;
      GMB.markDirty();
      drawGrid(); drawSignals(); validate();
    });
  }

  // POST /api/pins/validate takes the FULL profile and returns
  // { ok, issues:[{field,message,severity}] }. The firmware answers 422 (with
  // the issues in the body) when there is a blocking error, and 200 otherwise —
  // so issues can arrive via either the resolve or the reject path.
  function validate() {
    var p = GMB.state.profile;
    GMB.api.validatePins(p).then(function (res) {
      renderIssues((res && res.issues) || []);
    }).catch(function (e) {
      var body = e && e.body;
      if (body && body.issues) { renderIssues(body.issues); return; }
      var box = document.getElementById('pin-errors');
      if (box) { box.innerHTML = ''; box.appendChild(h('div.pill.error', 'Validation failed: ' + ((body && body.error) || e.message))); }
    });
  }

  function renderIssues(issues) {
    var box = document.getElementById('pin-errors');
    if (!box) return;
    box.innerHTML = '';
    if (!issues.length) { box.appendChild(h('div.pill.ok', 'All pins valid — no conflicts.')); return; }
    var blocking = issues.filter(function (i) { return i.severity !== 'warning'; });
    if (!blocking.length) box.appendChild(h('div.pill.ok', 'No blocking conflicts (warnings only).'));
    issues.forEach(function (e) {
      var warn = e.severity === 'warning';
      box.appendChild(h('div.err-item', [
        h('div.err-head', [h('strong', e.field || 'issue'),
          h('span.pill.mini.' + (warn ? 'warn' : 'error'), warn ? 'warning' : 'error')]),
        h('div.err-reason', e.message)
      ]));
    });
  }

  GMB.views.pins = { render: render, reset: function () { board = null; } };
})(window);
