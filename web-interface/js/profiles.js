/*
 * profiles.js — profiles page (spec section 20).
 *
 * Saved profiles live in numbered storage slots on the device
 * (GET /api/profiles -> { profiles:[{slot,name,used}], startupSlot }). Because
 * the firmware exposes no "read one slot" endpoint, copy / rename / set-startup
 * are composed client-side by loading the slot (which returns it as the active
 * profile), editing it and re-saving via POST /api/profiles.
 *
 * This page is the instrument LIBRARY and nothing else. Network settings belong
 * to the device and live in Settings > Network — there is exactly one editor for
 * them. Exports never include a Wi-Fi password (they are not part of the schema,
 * and the export strips the field defensively anyway).
 *
 * An import is normalised (GMB.ensureProfileDefaults) and then validated BY THE
 * DEVICE before it is adopted: the firmware owns the schema, the migration and
 * the cross-field rules, so a second, weaker validator here would only disagree
 * with it.
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

  // Read a slot's profile WITHOUT activating it (no homing / motor movement),
  // so copy / rename / set-startup are safe administrative actions.
  function fetchSlotProfile(slot) {
    return GMB.api.readProfileSlot(slot);
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
        GMB.navigate('fretboard');
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
  // (The Wi-Fi credential panel that used to live here is gone.) Network settings
  // belong to the DEVICE, not to the instrument library, and Settings > Network
  // is the one editor for them — it can also apply them without a reboot, which
  // this panel could not. Two editors for one setting is how the interface ended
  // up promising actions only one of them could perform.

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
        var obj;
        try {
          obj = JSON.parse(reader.result);
        } catch (e) { alert('Not valid JSON: ' + e.message); return; }
        // Shape check first, so an obviously wrong file fails immediately with a
        // readable message rather than through the API.
        var errs = validateImport(obj);
        if (errs.length) { alert('Invalid profile:\n- ' + errs.join('\n- ')); return; }
        if (!confirm('Load imported profile "' + (obj.instrument && obj.instrument.name) +
                     '" as the working profile?')) return;
        // Then hand it to the DEVICE's validator before adopting it. The firmware
        // owns the schema, the migration and the cross-field rules; the check
        // above only knows four keys, so a file missing board / midi /
        // stringFretSelection / hardware used to land in the UI unnoticed and
        // surface later as a mystery. The backend answers 422 with the reasons.
        GMB.api.validatePins(obj).then(function (res) {
          adoptImported(obj, (res && res.issues) || []);
        }).catch(function (e) {
          var body = e && e.body;
          if (body && body.issues) { adoptImported(obj, body.issues); return; }
          // The device is unreachable (offline demo): fall back to the local
          // validator rather than refusing to import at all, and say so.
          var local = GMB.validateProfile ? GMB.validateProfile(GMB.ensureProfileDefaults(obj)) : [];
          if (local.length) {
            alert('Invalid profile:\n- ' + local.join('\n- '));
            return;
          }
          adoptImported(obj, []);
        });
      };
      reader.readAsText(file);
    });
    document.body.appendChild(input); input.click(); input.remove();
  }

  // Adopt an imported profile as the working draft. Blocking issues refuse the
  // import; warnings are surfaced but let it through, so a profile that merely
  // needs attention can still be opened and fixed in the wizard.
  function adoptImported(obj, issues) {
    var blocking = issues.filter(function (i) { return i.severity !== 'warning'; });
    if (blocking.length) {
      alert('The device refused this profile:\n- ' +
            blocking.slice(0, 8).map(function (i) {
              return (i.field ? i.field + ' — ' : '') + i.message;
            }).join('\n- '));
      return;
    }
    GMB.state.profile = GMB.ensureProfileDefaults(obj);
    GMB.markDirty();
    if (issues.length) GMB.reportIssues('Imported with warnings', issues);
    GMB.toast('Profile imported. Review and save to publish.', 'ok');
    GMB.navigate('fretboard');
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
