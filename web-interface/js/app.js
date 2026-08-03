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
        if (v === null || v === undefined || v === false) return;
        if (k === 'class') el.className += (el.className ? ' ' : '') + v;
        else if (k === 'html') el.innerHTML = v;
        else if (k === 'text') el.textContent = v;
        else if (k.slice(0, 2) === 'on' && typeof v === 'function') el.addEventListener(k.slice(2), v);
        else if (k === 'value') el.value = v;
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
      (opts.options || []).forEach(function (o) {
        var val = o.value !== undefined ? o.value : o;
        var lab = o.label !== undefined ? o.label : o;
        el.appendChild(h('option', { value: val, selected: String(obj[key]) === String(val) }, lab));
      });
      el.value = obj[key];
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
      else if (type === 'number') v = el.value === '' ? null : Number(el.value);
      else v = el.value;
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
  var TABS = [
    { id: 'dashboard', label: 'Dashboard', icon: 'D' },
    { id: 'wizard', label: 'Setup Wizard', icon: 'W' },
    { id: 'pins', label: 'GPIO Pins', icon: 'P' },
    { id: 'midi', label: 'MIDI', icon: 'M' },
    { id: 'sysex', label: 'GMB / SysEx', icon: 'S' },
    { id: 'profiles', label: 'Profiles', icon: 'F' }
  ];

  var state = {
    profile: null,      // working draft (edited in place by views)
    mode: 'simplified', // 'simplified' | 'advanced' (spec 9.2)
    dirty: false,
    current: 'dashboard'
  };
  GMB.state = state;

  GMB.markDirty = function () {
    state.dirty = true;
    var b = document.getElementById('save-bar');
    if (b) b.classList.add('visible');
  };

  GMB.isAdvanced = function () { return state.mode === 'advanced'; };

  function setMode(mode) {
    state.mode = mode;
    document.body.setAttribute('data-mode', mode);
    var s = document.getElementById('mode-simplified'), a = document.getElementById('mode-advanced');
    if (s && a) { s.classList.toggle('active', mode === 'simplified'); a.classList.toggle('active', mode === 'advanced'); }
    render();
  }
  GMB.setMode = setMode;

  function navigate(id) {
    state.current = id;
    location.hash = '#' + id;
    document.querySelectorAll('.nav-item').forEach(function (n) {
      n.classList.toggle('active', n.getAttribute('data-tab') === id);
    });
    render();
  }
  GMB.navigate = navigate;

  function render() {
    var host = document.getElementById('view');
    if (!host) return;
    host.innerHTML = '';
    if (!state.profile) { host.appendChild(h('div.card', 'Loading configuration…')); return; }
    var view = GMB.views[state.current];
    if (view && view.render) {
      try { view.render(host); }
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
  GMB.saveProfile = function () {
    return GMB.api.putProfile(state.profile).then(function (res) {
      state.dirty = false;
      var b = document.getElementById('save-bar');
      if (b) b.classList.remove('visible');
      if (res && res.capabilitiesRevision) state.profile.capabilitiesRevision = res.capabilitiesRevision;
      GMB.toast('Profile saved (revision ' + (state.profile.capabilitiesRevision) + ').', 'ok');
      updateMockBadge();
    }).catch(function (e) {
      var body = e && e.body;
      if (body && body.issues && GMB.reportIssues('Save rejected', body.issues)) return;
      GMB.toast('Save failed: ' + ((body && body.error) || e.message), 'error');
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
        h('div.mode-toggle', [
          h('button#mode-simplified.active', { onclick: function () { setMode('simplified'); } }, 'Simplified'),
          h('button#mode-advanced', { onclick: function () { setMode('advanced'); } }, 'Advanced')
        ]),
        h('button.btn.danger.panic-side', { onclick: doPanic }, 'STOP')
      ])
    ]);

    var main = h('main.main', [
      h('header.topbar', [
        h('button.hamburger', { onclick: function () { nav.classList.toggle('open'); } }, '≡'),
        h('div#topbar-title.topbar-title', 'Dashboard'),
        h('div.topbar-right', [
          h('span#mock-badge.badge.mock', { style: 'display:none' }, 'DEMO / MOCK DATA'),
          h('span#conn-badge.badge.ok', 'Local')
        ])
      ]),
      h('div#view.view'),
      h('div#save-bar.save-bar', [
        h('span', 'You have unsaved changes.'),
        h('span.spacer'),
        GMB.button('Discard', function () { GMB.reloadProfile(); }, 'ghost'),
        GMB.button('Save & publish', function () { GMB.saveProfile(); }, 'primary')
      ])
    ]);

    app.appendChild(nav);
    app.appendChild(main);
    app.appendChild(h('div#toast-host.toast-host'));
  }

  function doPanic() {
    if (!confirm('PANIC / STOP: disable all drivers, neutralise servos and flush the MIDI queue. Continue?')) return;
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
    setMode('simplified');
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
