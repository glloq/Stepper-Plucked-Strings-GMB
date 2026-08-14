/*
 * app.js — application shell: routing between views, shared DOM helpers, the
 * global working-profile state, and the Simplified / Advanced mode toggle
 * (spec 9.2). Loaded after api.js and before the view modules.
 *
 * Each view module registers itself on GMB.views[name] with a render(container)
 * function. app.js owns navigation, the mode flag, and the draft profile that
 * views read and mutate; saving is atomic through GMB.api.putProfile.
 */
(function (global) {
  'use strict';
  var GMB = global.GMB;

  // ---- tiny DOM builder: h('div.class#id', {attrs}, [children]) -------------
  function h(tag, attrs, children) {
    var parts = tag.split(/(?=[.#])/);
    var el = document.createElement(parts[0] || 'div');
    parts.slice(1).forEach(function (p) {
      if (p[0] === '.') el.classList.add(p.slice(1));
      else if (p[0] === '#') el.id = p.slice(1);
    });
    if (attrs && (attrs.nodeType || typeof attrs === 'string' || Array.isArray(attrs))) {
      children = attrs; attrs = null;
    }
    if (attrs) {
      Object.keys(attrs).forEach(function (k) {
        var v = attrs[k];
        // `value` is a VALUE, not a boolean attribute, so it has to be handled
        // before the falsy guard below: `value:false` used to be dropped, and an
        // <option> with no value attribute falls back to its own text — so a
        // boolean select ("Active low" = false) could never match its model value
        // and painted blank. Empty string for null/undefined, as before.
        if (k === 'value') { el.value = (v === null || v === undefined) ? '' : v; return; }
        if (v === null || v === undefined || v === false) return;
        if (k === 'class') el.className += (el.className ? ' ' : '') + v;
        else if (k === 'html') el.innerHTML = v;
        else if (k === 'text') el.textContent = v;
        else if (k.slice(0, 2) === 'on' && typeof v === 'function') el.addEventListener(k.slice(2), v);
        else if (k === 'checked' || k === 'disabled' || k === 'selected') { if (v) el.setAttribute(k, k); el[k] = v; }
        else el.setAttribute(k, v);
      });
    }
    appendChildren(el, children);
    return el;
  }
  function appendChildren(el, children) {
    if (children === null || children === undefined) return;
    if (Array.isArray(children)) { children.forEach(function (c) { appendChildren(el, c); }); return; }
    if (children.nodeType) { el.appendChild(children); return; }
    el.appendChild(document.createTextNode(String(children)));
  }
  GMB.h = h;

  // Labelled form field helper.
  GMB.field = function (label, control, hint) {
    return h('label.field', [h('span.field-label', label), control,
      hint ? h('span.field-hint', hint) : null]);
  };

  // Bound input that writes obj[key] on change (with optional coercion).
  GMB.input = function (obj, key, opts) {
    opts = opts || {};
    var type = opts.type || 'text';
    var el;
    if (type === 'select') {
      el = h('select');
      var choices = opts.options || [];
      var matched = false;
      choices.forEach(function (o) {
        var val = o.value !== undefined ? o.value : o;
        var lab = o.label !== undefined ? o.label : o;
        var sel = String(obj[key]) === String(val);
        if (sel) matched = true;
        el.appendChild(h('option', { value: val, selected: sel }, lab));
      });
      if (matched) {
        el.value = obj[key];
      } else if (choices.length) {
        // The value is not in the list — almost always because the field is
        // undefined on a profile written before it existed. `el.value = undefined`
        // assigns the STRING "undefined", which matches no option, so the select
        // paints BLANK: the control looks broken and, worse, a save would store a
        // value the user never saw. Fall back to the first option and write it
        // into the model so what is on screen is what will be saved.
        var first = choices[0];
        var fv = first.value !== undefined ? first.value : first;
        el.selectedIndex = 0;
        obj[key] = opts.coerce ? opts.coerce(fv) : fv;
      }
    } else if (type === 'checkbox') {
      el = h('input', { type: 'checkbox', checked: !!obj[key] });
    } else {
      el = h('input', { type: type, value: obj[key] });
      if (opts.min !== undefined) el.min = opts.min;
      if (opts.max !== undefined) el.max = opts.max;
      if (opts.step !== undefined) el.step = opts.step;
      if (opts.placeholder) el.placeholder = opts.placeholder;
    }
    el.addEventListener('change', function () {
      var v;
      if (type === 'checkbox') v = el.checked;
      else if (type === 'number') {
        // A cleared number field coerces to its minimum (or 0), never null: a null
        // openNote/maxFret rendered "--" and turned fret loops into no-ops (G10).
        if (el.value === '') { v = (opts.min !== undefined) ? opts.min : 0; el.value = v; }
        else v = Number(el.value);
      } else v = el.value;
      if (opts.coerce) v = opts.coerce(v);
      obj[key] = v;
      if (opts.onChange) opts.onChange(v);
      GMB.markDirty();
    });
    if (opts.disabled) el.disabled = true;
    return el;
  };

  GMB.button = function (label, onClick, cls) {
    return h('button.btn' + (cls ? '.' + cls : ''), { type: 'button', onclick: onClick }, label);
  };

  // Surface a list of backend validation issues ({field,message,severity}) as
  // toasts. Used by save (422), pin validation and SysEx publishing.
  GMB.reportIssues = function (prefix, issues) {
    if (!issues || !issues.length) return false;
    GMB.toast(prefix + ': ' + issues.length + ' issue(s).', 'error');
    issues.forEach(function (is) {
      var warn = is.severity === 'warning';
      GMB.toast((warn ? '⚠ ' : '✖ ') + (is.field ? is.field + ' — ' : '') + is.message, warn ? 'warn' : 'error');
    });
    return true;
  };

  // Toast notifications.
  GMB.toast = function (msg, kind) {
    var host = document.getElementById('toast-host');
    if (!host) return;
    var t = h('div.toast' + (kind ? '.' + kind : ''), msg);
    host.appendChild(t);
    setTimeout(function () { t.classList.add('show'); }, 10);
    setTimeout(function () { t.classList.remove('show'); setTimeout(function () { t.remove(); }, 300); }, 3600);
  };

  // ---- routing --------------------------------------------------------------
  // Three main pages: the playable Instrument (live carriage positions, click a
  // fret to hear it), the complete Setup flow (the whole instrument creation in
  // order — identity, axes & transmission, board, homing, frets, plucking, MIDI,
  // timing, test, validation), and the Wiring & GPIO reference (harness, power &
  // safety, I²C, pins, commissioning). Only device Wi-Fi and the diagnostic tools
  // (SysEx / MIDI monitor / diagnostics) live in the Settings modal (gear button,
  // top-right).
  var TABS = [
    { id: 'fretboard', label: 'Instrument', icon: '♪' },
    { id: 'wizard', label: 'Setup', icon: '⛭' },
    { id: 'hardware', label: 'Wiring & GPIO', icon: '⚡' }
  ];

  var state = {
    profile: null,      // working draft (edited in place by views)
    mode: 'detailed',   // one mode; the toggle of spec 9.2 was removed
    dirty: false,
    current: 'fretboard'
  };
  GMB.state = state;

  GMB.markDirty = function () {
    state.dirty = true;
    var b = document.getElementById('save-bar');
    if (b) b.classList.add('visible');
  };

  // The expert-mode TOGGLE is gone: there is one interface, and it is the detailed
  // one. Views still ask this before rendering their fine-tuning blocks (steps per
  // revolution, homing speeds, servo pulse windows, the SysEx block switches…), so
  // it answers TRUE — returning false would not "simplify" anything, it would make
  // that content permanently unreachable, which is precisely what a bench needs.
  // Kept as a function so an old profile carrying a `mode` field is harmless.
  GMB.isAdvanced = function () { return true; };
  // There is one mode now. Kept so the body attribute (and any CSS keyed on it)
  // stays defined, and so an old caller does not throw.
  function setMode() { state.mode = 'detailed'; document.body.setAttribute('data-mode', 'detailed'); }
  GMB.setMode = setMode;

  function navigate(id) {
    // Leaving a view must never leave a group test driving the servos in the
    // background: cancel any running sequence before switching.
    if (GMB.testRunner) GMB.testRunner.stop();
    state.current = id;
    location.hash = '#' + id;
    document.querySelectorAll('.nav-item').forEach(function (n) {
      n.classList.toggle('active', n.getAttribute('data-tab') === id);
    });
    render();
  }
  GMB.navigate = navigate;

  var mountedView = null;   // id of the view currently in the DOM (for teardown)

  function render() {
    var host = document.getElementById('view');
    if (!host) return;
    // Let the outgoing view release anything live (sockets, servo holds, timers)
    // before it is torn out of the DOM. Runs on every re-render, so a view's
    // teardown must be idempotent — its render then re-establishes what it needs.
    if (mountedView && GMB.views[mountedView] && GMB.views[mountedView].teardown) {
      try { GMB.views[mountedView].teardown(); } catch (e) {}
    }
    mountedView = null;
    host.innerHTML = '';
    if (!state.profile) { host.appendChild(h('div.card', 'Loading configuration…')); return; }
    var view = GMB.views[state.current];
    if (view && view.render) {
      try { view.render(host); mountedView = state.current; }
      catch (e) { host.appendChild(h('div.card', [h('h2', 'View error'), h('pre', String(e && e.stack || e))])); }
    } else {
      host.appendChild(h('div.card', 'Unknown view: ' + state.current));
    }
    updateMockBadge();
  }
  GMB.render = render;

  function updateMockBadge() {
    var badge = document.getElementById('mock-badge');
    if (badge) badge.style.display = GMB.api.mock ? 'inline-flex' : 'none';
  }
  GMB.updateMockBadge = updateMockBadge;

  // Save the working draft atomically (SysEx spec 15: only validated profiles
  // are published; a save increments capabilitiesRevision on the backend).
  // The returned Promise REJECTS on failure (after toasting): callers that chain
  // follow-up actions on a successful save must be able to tell the difference —
  // the old swallow-and-resolve let "saved, now do X" run on a failed save
  // (audit 4). Callers that don't care can ignore the rejection via .catch.
  // PUT /api/profile answers 202: the activation is only QUEUED (the loop parks the
  // old servos, swaps, re-parks, re-arms). "Saved" therefore is NOT "active": after
  // acceptance we follow the command outcome and then poll the status until the
  // instrument is back to ready/readyDegraded, so the success toast means the new
  // profile actually RUNS (audit 5). Timeouts degrade to an honest "still
  // activating" warning without rejecting (the activation continues on-device).
  function waitForActivation(commandId) {
    function delay(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }
    // The firmware now reports the WHOLE activation on the command itself
    // (audit 7): "running" while the old profile parks, swaps and the new one
    // re-parks; "succeeded" only when the new profile really reached ready.
    // So the command poll carries the ~30 s budget (double park + arming), and
    // the status poll after it is a short confirmation, not a guess.
    function pollCommand(triesLeft) {
      if (!commandId) return Promise.resolve('succeeded');  // mock / immediate path
      return GMB.api.commandState(commandId).then(function (r) {
        var st = r && r.state;
        if (st === 'succeeded') return 'succeeded';
        if (st === 'refused') throw Object.assign(new Error('activation refused'),
                                                  { refused: true });
        // Purged by a panic / E-stop before it ran (audit 6): stop immediately
        // instead of polling a ghost to the timeout.
        if (st === 'cancelled') throw Object.assign(new Error('activation cancelled'),
                                                    { refused: true, cancelled: true });
        // The swap started but could not be completed (park unconfirmed, arming
        // failed): terminal — the device kept the safest posture (audit 7).
        if (st === 'failed') throw Object.assign(new Error('activation failed'),
                                                 { refused: true, failed: true });
        if (triesLeft <= 0) return 'timeout';
        return delay(500).then(function () { return pollCommand(triesLeft - 1); });
      });
    }
    function pollReady(triesLeft) {
      return GMB.api.getStatus().then(function (st) {
        var s = String((st && st.state) || '').toLowerCase();
        if (s === 'ready' || s === 'readydegraded') return 'ready';
        if (triesLeft <= 0) return 'timeout';
        return delay(500).then(function () { return pollReady(triesLeft - 1); });
      });
    }
    return pollCommand(60).then(function (r) {
      if (r === 'timeout') return 'timeout';
      return pollReady(10);
    });
  }

  GMB.saveProfile = function () {
    // `dirty` is only cleared once the activation is CONFIRMED (or on the mock /
    // legacy immediate path): the 202 merely queues it, and the command can still
    // be refused (safety lock) or cancelled (panic purge) before it runs — the
    // draft must then keep showing as unsaved (audit 6).
    function markSaved() {
      state.dirty = false;
      var b = document.getElementById('save-bar');
      if (b) b.classList.remove('visible');
    }
    return GMB.api.putProfile(state.profile).then(function (res) {
      if (res && res.capabilitiesRevision) state.profile.capabilitiesRevision = res.capabilitiesRevision;
      updateMockBadge();
      if (res && res.accepted !== undefined) {
        GMB.toast('Profile accepted — activating…', 'ok');
        return waitForActivation(res.commandId).then(function (r) {
          if (r === 'timeout') {
            GMB.toast('Activation still in progress — the draft stays marked ' +
                      'unsaved until it is confirmed.', 'warn');
          } else {
            markSaved();
            GMB.toast('Profile published and ACTIVE (revision ' +
                      state.profile.capabilitiesRevision + ').', 'ok');
          }
        });
      }
      // Mock / legacy backend: no queue — the save is the whole story.
      markSaved();
      GMB.toast('Profile saved (revision ' + (state.profile.capabilitiesRevision) + ').', 'ok');
    }).catch(function (e) {
      var body = e && e.body;
      if (e && e.cancelled)
        GMB.toast('Activation cancelled (panic / E-stop / safety stop) — the draft ' +
                  'is still unsaved.', 'error');
      else if (e && e.failed)
        GMB.toast('Activation FAILED on the device (parking could not be confirmed ' +
                  'or arming failed) — check Diagnostics; the draft is still unsaved.',
                  'error');
      else if (e && e.refused)
        GMB.toast('Activation refused by the device (safety locked or invalid profile).', 'error');
      else if (!(body && body.issues && GMB.reportIssues('Save rejected', body.issues)))
        GMB.toast('Save failed: ' + ((body && body.error) || e.message), 'error');
      throw e;  // the toast is shown; callers still need the real outcome
    });
  };

  GMB.reloadProfile = function () {
    return GMB.api.getProfile().then(function (p) {
      state.profile = p;
      state.dirty = false;
      var b = document.getElementById('save-bar');
      if (b) b.classList.remove('visible');
      render();
    });
  };

  // ---- shell construction ---------------------------------------------------
  function buildShell() {
    var app = document.getElementById('app');
    app.innerHTML = '';

    var nav = h('nav.sidebar', [
      h('div.brand', [h('div.brand-mark', 'GMB'),
        h('div.brand-text', [h('strong', 'Stepper-Plucked'), h('small', 'Strings-GMB')])]),
      h('div.nav-list', TABS.map(function (t) {
        return h('button.nav-item', {
          'data-tab': t.id, onclick: function () { navigate(t.id); },
          class: t.id === state.current ? 'active' : ''
        }, [h('span.nav-icon', t.icon), h('span.nav-label', t.label)]);
      })),
      h('div.nav-footer', [
        h('button.btn.danger.panic-side', { onclick: doPanic }, 'STOP')
      ])
    ]);

    var main = h('main.main', [
      h('header.topbar', [
        h('button.hamburger', { onclick: function () { nav.classList.toggle('open'); } }, '≡'),
        h('div#topbar-title.topbar-title', 'Instrument'),
        h('div.topbar-right', [
          h('span#mock-badge.badge.mock', { style: 'display:none' }, 'DEMO / MOCK DATA'),
          h('span#conn-badge.badge.ok', 'Local'),
          h('button.icon-btn#settings-btn', {
            title: 'Device settings — Wi-Fi network and advanced diagnostic tools',
            onclick: function () { if (GMB.openSettings) GMB.openSettings(); }
          }, '⚙')
        ])
      ]),
      h('div#view.view'),
      h('div#save-bar.save-bar', [
        h('span', 'You have unsaved changes.'),
        h('span.spacer'),
        GMB.button('Discard', function () { GMB.reloadProfile(); }, 'ghost'),
        GMB.button('Save & publish', function () { GMB.saveProfile().catch(function () {}); }, 'primary')
      ])
    ]);

    app.appendChild(nav);
    app.appendChild(main);
    app.appendChild(h('div#toast-host.toast-host'));
  }

  function doPanic() {
    if (!confirm('PANIC / STOP: disable all drivers, neutralise servos and flush the MIDI queue. Continue?')) return;
    if (GMB.testRunner) GMB.testRunner.stop();   // halt any client-side group test too
    GMB.api.panic().then(function (r) { GMB.toast(r.message || 'Panic executed.', 'warn'); });
  }
  GMB.doPanic = doPanic;

  // Keep the topbar title in sync with the active tab.
  var _navigate = navigate;
  navigate = function (id) {
    _navigate(id);
    var title = (TABS.filter(function (t) { return t.id === id; })[0] || {}).label || id;
    var el = document.getElementById('topbar-title');
    if (el) el.textContent = title;
    var side = document.querySelector('.sidebar');
    if (side) side.classList.remove('open');
  };
  GMB.navigate = navigate;

  // ---- boot -----------------------------------------------------------------
  function boot() {
    buildShell();
    setMode();
    GMB.api.getProfile().then(function (p) {
      state.profile = p;
      var start = (location.hash || '').replace('#', '');
      if (TABS.some(function (t) { return t.id === start; })) state.current = start;
      navigate(state.current);
      updateMockBadge();
    });
  }

  window.addEventListener('hashchange', function () {
    var id = (location.hash || '').replace('#', '');
    if (id && id !== state.current && TABS.some(function (t) { return t.id === id; })) navigate(id);
  });

  document.addEventListener('DOMContentLoaded', boot);

  GMB.views = GMB.views || {};
})(window);
