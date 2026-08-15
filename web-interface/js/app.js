/*
 * app.js — application shell: routing between views, shared DOM helpers and the
 * global working-profile state. Loaded after api.js and before the view modules.
 *
 * There is no Simplified / Advanced mode toggle. It was removed: disclosure is
 * LOCAL to each step (GMB.details), because a global mode makes the one field you
 * need at the bench unreachable and doubles every page into two variants to
 * maintain. GMB.isAdvanced() survives only as a `true` stub for callers not yet
 * cleaned up.
 *
 * Each view module registers itself on GMB.views[name] with a render(container)
 * function. app.js owns navigation and the draft profile that views read and
 * mutate; publishing goes through GMB.saveProfile -> PUT /api/profile, which both
 * persists and activates.
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
  // Exposed so a view can refresh a live sub-block in place (the Instrument
  // dashboard repaints on every status frame) without rebuilding the page.
  GMB.appendChildren = appendChildren;

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

  // A MIDI channel field. Shows 1–16, stores 0–15.
  //
  // The field was labelled "Global channel (1–16)" over an input with min=0 max=15
  // and the note "stored zero-based". That asks the user to know an implementation
  // detail in order to enter their own channel correctly — and to type 0 when their
  // sequencer says 1. The wire format is unchanged; only the conversion moved to
  // where it belongs.
  GMB.channelInput = function (obj, key, opts) {
    opts = opts || {};
    var el = h('input', { type: 'number', value: ((obj[key] | 0) + 1) });
    el.min = 1;
    el.max = 16;
    el.addEventListener('change', function () {
      var shown = el.value === '' ? 1 : Number(el.value);
      if (!(shown >= 1)) shown = 1;
      if (shown > 16) shown = 16;
      el.value = shown;
      obj[key] = shown - 1;          // stored zero-based, as the firmware expects
      if (opts.onChange) opts.onChange(obj[key]);
      GMB.markDirty();
    });
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

  // ---- progressive disclosure ------------------------------------------------
  //
  // The old global Simplified / Advanced toggle is gone, and putting it back would
  // be the wrong shape: a mode that hides content everywhere means the one field
  // you need at the bench is unreachable, and it doubles every page into two
  // variants to maintain. Disclosure is LOCAL instead — each step shows the few
  // parameters you must decide, and parks the fine-tuning in a block you open on
  // the spot. Nothing is ever unreachable; it is just not in the way.
  //
  // Open/closed state is per block and remembered for the session, so a bench user
  // who opens "Homing speeds" once keeps it open while they work through the
  // strings. It is deliberately NOT persisted to the profile: it is a view
  // preference, not instrument configuration.
  var disclosed = {};
  GMB.details = function (key, title, buildBody, opts) {
    opts = opts || {};
    var open = disclosed[key] !== undefined ? disclosed[key] : !!opts.open;
    var wrap = h('div.disclose' + (open ? '.open' : ''));
    var body = h('div.disclose-body');
    var caret = h('span.disclose-caret', open ? '▾' : '▸');
    var head = h('button.disclose-head', { type: 'button' }, [
      caret, h('span.disclose-title', title),
      opts.hint ? h('span.muted.disclose-hint', opts.hint) : null
    ]);
    var built = false;
    function fill() {
      if (built) return;
      built = true;
      // Built on first open: a closed block costs nothing, which matters on the
      // per-string steps where several of these exist at once.
      var kids = buildBody();
      if (kids) appendChildren(body, kids);
    }
    head.addEventListener('click', function () {
      open = !open;
      disclosed[key] = open;
      wrap.classList.toggle('open', open);
      caret.textContent = open ? '▾' : '▸';
      if (open) fill();
    });
    if (open) fill();
    wrap.appendChild(head);
    wrap.appendChild(body);
    return wrap;
  };
  // Old callers asked this before rendering a fine-tuning block. Everything is
  // reachable now (behind GMB.details), so it answers true; kept so an old profile
  // carrying a `mode` field, or a stale call, is harmless.
  GMB.isAdvanced = function () { return true; };
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
      // No id = the MOCK / legacy backend, which applies synchronously. On the real
      // device a publish that reports no command id also reports
      // outcome:"storedNotActivated", and saveProfile() handles that before ever
      // getting here — reaching this line with a 0 from the device would confirm
      // against a status that is still Ready for the OLD profile.
      if (!commandId) return Promise.resolve('succeeded');
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

  // Follow an accepted command through to the machine actually being Ready.
  //
  // Exposed because "the device said 202" is not "the device did it": an activation
  // spans park -> swap -> re-home -> Ready over many loop passes. Save & publish
  // already waited; Load profile did not, and re-read the profile immediately after
  // the 202 — so it could show the OLD instrument under a toast saying the new one
  // had loaded.
  GMB.followCommand = function (commandId) { return waitForActivation(commandId); };

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
      // Stored, but the activation was never queued. The profile IS on flash — the
      // next boot runs it — so the draft is genuinely saved; it is simply not
      // running yet. This case used to fall into waitForActivation(0), which
      // shortcuts to "succeeded" and then confirmed against a status that was
      // still Ready FOR THE OLD PROFILE, so the UI announced "published and
      // ACTIVE" for a profile the machine had never loaded.
      if (res && res.outcome === 'storedNotActivated') {
        markSaved();
        GMB.toast('Saved, but NOT activated now: it takes effect at the next reboot. ' +
                  ((res && res.error) || ''), 'warn');
        return;
      }
      if (res && res.accepted !== undefined) {
        GMB.toast('Profile accepted — activating…', 'ok');
        return waitForActivation(res.commandId).then(function (r) {
          if (r === 'timeout') {
            GMB.toast('Activation still in progress — the draft stays marked ' +
                      'unsaved until it is confirmed.', 'warn');
          } else {
            // No "active but not saved" case to handle any more. The device now
            // commits the write BEFORE the activation becomes runnable, so an
            // accepted publish is a persisted one: `accepted` and `persisted` are
            // true together or the request failed outright and this is the catch
            // block's problem. There used to be a branch here for the third state,
            // and a branch for a state that cannot occur is a claim that it can.
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
      // The commit happens BEFORE the activation is queued, so a refusal here is
      // not "nothing was saved" — the profile is on flash and the next boot runs
      // it. What failed is the swap on a running machine, which a latched E-stop
      // or an unconfirmed park can legitimately refuse. Saying "still unsaved"
      // would send the operator to re-publish something already stored.
      var storedAnyway = ' The configuration IS stored: it will be active after a ' +
                         'reboot, or after a successful Reset & re-home.';
      if (e && e.cancelled)
        GMB.toast('Activation cancelled (panic / E-stop / safety stop).' + storedAnyway,
                  'warn');
      else if (e && e.failed)
        GMB.toast('Activation FAILED on the device (parking could not be confirmed ' +
                  'or arming failed) — check Diagnostics.' + storedAnyway, 'error');
      else if (e && e.refused)
        GMB.toast('Activation refused by the device (safety locked).' + storedAnyway,
                  'warn');
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
