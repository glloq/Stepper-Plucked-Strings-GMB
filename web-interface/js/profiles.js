/*
 * profiles.js — profiles page (cahier des charges section 20).
 *
 * Saved profiles live in numbered storage slots on the device
 * (GET /api/profiles -> { profiles:[{slot,name,used}], startupSlot }). Because
 * the firmware exposes no "read one slot" endpoint, copy / rename / set-startup
 * are composed client-side by loading the slot (which returns it as the active
 * profile), editing it and re-saving via POST /api/profiles. Exports never
 * include the Wi-Fi password (section 20 / SysEx spec 20); Wi-Fi credentials
 * are set write-only through POST /api/wifi.
 */
(function (global) {
  'use strict';
  var GMB = global.GMB, h = GMB.h;

  var lastList = { profiles: [], startupSlot: 0 };

  function render(host) {
    host.appendChild(h('div.card', [
      h('div.card-head', [h('h2', 'Saved profiles'), h('span.muted', 'device storage slots')]),
      h('div#profile-list.profile-list', 'Loading…'),
      h('div.toolbar', [
        GMB.button('Save working profile to a free slot', createProfile, 'primary'),
        GMB.button('Import JSON…', importProfile, 'ghost'),
        GMB.button('Export current', function () { exportProfile(GMB.state.profile); }, 'ghost')
      ])
    ]));

    host.appendChild(h('div.card', [
      h('h2', 'Current working profile'),
      h('p.muted', 'Edited across the Wizard, Pins and MIDI tabs; saved atomically.'),
      h('div.form-grid', [
        GMB.field('Name', GMB.input(GMB.state.profile.instrument, 'name')),
        GMB.field('Startup profile', h('span.muted', 'set from the list above'))
      ]),
      h('div.toolbar', [GMB.button('Save & publish', function () { GMB.saveProfile(); }, 'primary')])
    ]));

    host.appendChild(wifiCard());

    loadList();
  }

  function loadList() {
    GMB.api.getProfiles().then(function (data) {
      lastList = data || { profiles: [], startupSlot: 0 };
      var box = document.getElementById('profile-list');
      if (!box) return;
      box.innerHTML = '';
      var profiles = lastList.profiles || [];
      profiles.forEach(function (pr) {
        var isStartup = pr.slot === lastList.startupSlot;
        if (!pr.used) {
          box.appendChild(h('div.profile-item.empty', [
            h('div.profile-main', [
              h('div.profile-name', [h('strong', 'Slot ' + pr.slot), h('span.muted', ' — empty')])
            ]),
            h('div.profile-actions', [
              GMB.button('Save working profile here', function () { saveDraftToSlot(pr.slot, false); }, 'ghost')
            ])
          ]));
          return;
        }
        box.appendChild(h('div.profile-item', [
          h('div.profile-main', [
            h('div.profile-name', [h('strong', pr.name || '(unnamed)'),
              isStartup ? h('span.pill.mini.ok', 'startup') : null]),
            h('div.muted', 'slot ' + pr.slot)
          ]),
          h('div.profile-actions', [
            GMB.button('Load', function () { loadProfile(pr); }, 'ghost'),
            GMB.button('Copy', function () { copyProfile(pr); }, 'ghost'),
            GMB.button('Rename', function () { renameProfile(pr); }, 'ghost'),
            GMB.button('Startup', function () { setStartup(pr); }, 'ghost'),
            GMB.button('Delete', function () { deleteProfile(pr); }, 'danger-ghost')
          ])
        ]));
      });
      if (!profiles.length) box.appendChild(h('div.muted', 'No slots reported.'));
    }).catch(function (e) { reportErr('Could not load profiles', e); });
  }

  function firstFreeSlot() {
    var free = (lastList.profiles || []).filter(function (p) { return !p.used; })[0];
    return free ? free.slot : -1;
  }

  // Load a slot and return its profile (the firmware returns it as the newly
  // active profile — there is no read-only slot endpoint).
  function fetchSlotProfile(slot) {
    return GMB.api.loadProfileSlot(slot).then(function (res) {
      if (res && res.ok === false) throw new Error('slot ' + slot + ' is empty');
      return GMB.api.getProfile();
    });
  }

  function saveDraftToSlot(slot, startup) {
    return GMB.api.saveProfileSlot(slot, GMB.deepCopy(GMB.state.profile), startup).then(function (res) {
      if (res && res.ok === false) { GMB.toast('Save to slot ' + slot + ' failed.', 'error'); return; }
      GMB.toast('Working profile saved to slot ' + slot + '.', 'ok');
      loadList();
    }).catch(function (e) { reportErr('Save to slot failed', e); });
  }

  function createProfile() {
    var free = firstFreeSlot();
    if (free < 0) { alert('All slots are full. Delete one first.'); return; }
    var name = prompt('Name for the saved profile:', GMB.state.profile.instrument.name || 'New instrument');
    if (!name) return;
    var prof = GMB.deepCopy(GMB.state.profile);
    prof.instrument.name = name;
    GMB.api.saveProfileSlot(free, prof, false).then(function (res) {
      if (res && res.ok === false) { GMB.toast('Save failed.', 'error'); return; }
      GMB.toast('Saved "' + name + '" to slot ' + free + '.', 'ok');
      loadList();
    }).catch(function (e) { reportErr('Save failed', e); });
  }

  function copyProfile(pr) {
    var free = firstFreeSlot();
    if (free < 0) { alert('No free slot to copy into.'); return; }
    var name = prompt('Name for the copy:', (pr.name || 'profile') + ' copy');
    if (!name) return;
    fetchSlotProfile(pr.slot).then(function (prof) {
      prof.instrument.name = name;
      return GMB.api.saveProfileSlot(free, prof, false);
    }).then(function (res) {
      if (res && res.ok === false) { GMB.toast('Copy failed.', 'error'); return; }
      GMB.toast('Copied "' + pr.name + '" into slot ' + free + '.', 'ok');
      loadList();
    }).catch(function (e) { reportErr('Copy failed', e); });
  }

  function renameProfile(pr) {
    var name = prompt('Rename profile:', pr.name || '');
    if (!name) return;
    fetchSlotProfile(pr.slot).then(function (prof) {
      prof.instrument.name = name;
      return GMB.api.saveProfileSlot(pr.slot, prof, pr.slot === lastList.startupSlot);
    }).then(function (res) {
      if (res && res.ok === false) { GMB.toast('Rename failed.', 'error'); return; }
      GMB.toast('Renamed slot ' + pr.slot + ' to "' + name + '".', 'ok');
      loadList();
    }).catch(function (e) { reportErr('Rename failed', e); });
  }

  function deleteProfile(pr) {
    if (!confirm('Delete profile "' + pr.name + '" (slot ' + pr.slot + ')?')) return;
    GMB.api.deleteProfileSlot(pr.slot).then(function (res) {
      if (res && res.ok === false) { GMB.toast('Delete failed.', 'error'); return; }
      GMB.toast('Deleted slot ' + pr.slot + '.', 'ok');
      loadList();
    }).catch(function (e) { reportErr('Delete failed', e); });
  }

  // Set startup = re-save the slot with startup:true (no dedicated endpoint).
  function setStartup(pr) {
    fetchSlotProfile(pr.slot).then(function (prof) {
      return GMB.api.saveProfileSlot(pr.slot, prof, true);
    }).then(function (res) {
      if (res && res.ok === false) { GMB.toast('Could not set startup slot.', 'error'); return; }
      GMB.toast('"' + pr.name + '" set as the startup profile.', 'ok');
      loadList();
    }).catch(function (e) { reportErr('Set startup failed', e); });
  }

  function loadProfile(pr) {
    GMB.api.loadProfileSlot(pr.slot).then(function (res) {
      if (res && res.ok === false) { GMB.toast('Slot ' + pr.slot + ' is empty.', 'warn'); return; }
      GMB.reloadProfile().then(function () {
        GMB.toast('Loaded "' + pr.name + '".', 'ok');
        GMB.navigate('dashboard');
      });
    }).catch(function (e) { reportErr('Load failed', e); });
  }

  // Show backend validation issues (422) or a plain error message.
  function reportErr(prefix, e) {
    var body = e && e.body;
    if (body && body.issues && GMB.reportIssues(prefix, body.issues)) return;
    GMB.toast(prefix + ': ' + ((body && body.error) || (e && e.message) || 'error'), 'error');
  }

  // ---- Wi-Fi credentials (write-only) --------------------------------------
  var wifi = { stationPassword: '', apPassword: '' };
  function wifiCard() {
    return h('div.card', [
      h('div.card-head', [h('h2', 'Wi-Fi credentials'),
        h('span.muted', 'write-only — never exported or displayed')]),
      h('p.muted', 'Passwords are stored on the device and applied after a reboot. Leaving a field blank leaves that password unchanged.'),
      h('div.form-grid', [
        GMB.field('Station (client) password', GMB.input(wifi, 'stationPassword', { type: 'password' })),
        GMB.field('Access-point password', GMB.input(wifi, 'apPassword', { type: 'password' }))
      ]),
      h('div.toolbar', [GMB.button('Save Wi-Fi credentials', saveWifi, 'primary')])
    ]);
  }
  function saveWifi() {
    GMB.api.setWifi({ stationPassword: wifi.stationPassword, apPassword: wifi.apPassword }).then(function (res) {
      if (res && res.ok === false) { GMB.toast('Could not store Wi-Fi credentials.', 'error'); return; }
      wifi.stationPassword = ''; wifi.apPassword = '';
      GMB.toast('Wi-Fi credentials stored' + (res && res.note ? ' — ' + res.note : '') + '.', 'ok');
      GMB.render();
    }).catch(function (e) { reportErr('Wi-Fi save failed', e); });
  }

  // Export — strips the Wi-Fi password (never present in our schema, but we also
  // guard against a passworded field) and downloads pretty JSON.
  function exportProfile(profile) {
    var copy = GMB.deepCopy(profile);
    if (copy.network) { delete copy.network.password; delete copy.network.stationPassword; delete copy.network.apPassword; }
    var blob = new Blob([JSON.stringify(copy, null, 2)], { type: 'application/json' });
    var url = URL.createObjectURL(blob);
    var a = h('a', { href: url, download: (GMB.slug(profile.instrument.name) || 'profile') + '.json' });
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(function () { URL.revokeObjectURL(url); }, 1000);
    GMB.toast('Profile exported (Wi-Fi password excluded).', 'ok');
  }

  function importProfile() {
    var input = h('input', { type: 'file', accept: '.json,application/json', style: 'display:none' });
    input.addEventListener('change', function () {
      var file = input.files[0];
      if (!file) return;
      var reader = new FileReader();
      reader.onload = function () {
        try {
          var obj = JSON.parse(reader.result);
          var errs = validateImport(obj);
          if (errs.length) { alert('Invalid profile:\n- ' + errs.join('\n- ')); return; }
          if (confirm('Load imported profile "' + (obj.instrument && obj.instrument.name) + '" as the working profile?')) {
            GMB.state.profile = obj;
            GMB.markDirty();
            GMB.toast('Profile imported. Review and save to publish.', 'ok');
            GMB.navigate('dashboard');
          }
        } catch (e) { alert('Not valid JSON: ' + e.message); }
      };
      reader.readAsText(file);
    });
    document.body.appendChild(input); input.click(); input.remove();
  }

  function validateImport(obj) {
    var errs = [];
    if (!obj || typeof obj !== 'object') { errs.push('Not an object.'); return errs; }
    if (obj.project !== 'Stepper-Plucked-Strings-GMB') errs.push('Wrong project tag.');
    if (!obj.instrument) errs.push('Missing "instrument".');
    if (!Array.isArray(obj.strings)) errs.push('Missing "strings" array.');
    if (obj.instrument && (obj.instrument.stringCount < 1 || obj.instrument.stringCount > 6)) errs.push('String count out of range.');
    return errs;
  }

  GMB.views.profiles = { render: render };
})(window);
