/*
 * profiles.js — profiles page (cahier des charges section 20).
 *
 * List / create / copy / rename / delete / export / import(JSON) / restore /
 * set-startup. Exports never include the Wi-Fi password (section 20 / SysEx
 * spec 20). Import validates the shape before offering to load it.
 */
(function (global) {
  'use strict';
  var GMB = global.GMB, h = GMB.h;

  function render(host) {
    host.appendChild(h('div.card', [
      h('div.card-head', [h('h2', 'Saved profiles'), h('span.muted', 'at least 8 slots')]),
      h('div#profile-list.profile-list', 'Loading…'),
      h('div.toolbar', [
        GMB.button('New profile', createProfile, 'primary'),
        GMB.button('Import JSON…', importProfile, 'ghost'),
        GMB.button('Export current', function () { exportProfile(GMB.state.profile); }, 'ghost')
      ])
    ]));

    host.appendChild(h('div.card', [
      h('h2', 'Current working profile'),
      h('p.muted', 'Edited across the Wizard, Pins and MIDI tabs; saved atomically.'),
      h('div.form-grid', [
        GMB.field('Name', GMB.input(GMB.state.profile.instrument, 'name')),
        GMB.field('Startup profile', h('span.muted', 'set from the list below'))
      ]),
      h('div.toolbar', [GMB.button('Save & publish', function () { GMB.saveProfile(); }, 'primary')])
    ]));

    loadList();
  }

  function loadList() {
    GMB.api.getProfiles().then(function (list) {
      var box = document.getElementById('profile-list');
      if (!box) return;
      box.innerHTML = '';
      list.forEach(function (pr) {
        box.appendChild(h('div.profile-item', [
          h('div.profile-main', [
            h('div.profile-name', [h('strong', pr.name), pr.startup ? h('span.pill.mini.ok', 'startup') : null]),
            h('div.muted', pr.type + ' · ' + pr.stringCount + ' strings')
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
      if (!list.length) box.appendChild(h('div.muted', 'No profiles.'));
    });
  }

  function createProfile() {
    var name = prompt('New profile name:', 'New instrument');
    if (!name) return;
    GMB.api.postProfiles('create', { name: name, stringCount: 4, type: 'ukulele' }).then(loadList);
  }
  function copyProfile(pr) {
    var name = prompt('Copy name:', pr.name + ' copy');
    if (!name) return;
    GMB.api.postProfiles('copy', { id: pr.id, name: name }).then(loadList);
  }
  function renameProfile(pr) {
    var name = prompt('Rename profile:', pr.name);
    if (!name) return;
    GMB.api.postProfiles('rename', { id: pr.id, name: name }).then(loadList);
  }
  function deleteProfile(pr) {
    if (!confirm('Delete profile "' + pr.name + '"?')) return;
    GMB.api.postProfiles('delete', { id: pr.id }).then(loadList);
  }
  function setStartup(pr) {
    GMB.api.postProfiles('setStartup', { id: pr.id }).then(function () {
      GMB.toast('"' + pr.name + '" set as startup profile.', 'ok'); loadList();
    });
  }
  function loadProfile(pr) {
    // In mock mode there is a single working profile; a real backend would load
    // the selected slot. We reload the active profile for consistency.
    GMB.reloadProfile().then(function () { GMB.toast('Loaded "' + pr.name + '".', 'ok'); GMB.navigate('dashboard'); });
  }

  // Export — strips the Wi-Fi password (never present in our schema, but we also
  // guard against a passworded field) and downloads pretty JSON.
  function exportProfile(profile) {
    var copy = GMB.deepCopy(profile);
    if (copy.network) { delete copy.network.password; }
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
