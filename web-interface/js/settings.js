/*
 * settings.js — the Settings modal (gear button, top-right).
 *
 * Building an instrument lives on the Setup page. This modal holds everything
 * that is NOT that, split strictly by what a thing belongs to — mixing the three
 * is what produced two editors for the Wi-Fi and a profile library no route
 * reached:
 *
 *   Profiles     the instrument LIBRARY: slots, import/export.
 *   Network      device link: mode, SSIDs, hostname, write-only credentials,
 *                the on-demand hotspot. Stays with the machine, not the tune.
 *   Security     device: admin token and the network-MIDI source policy.
 *   Diagnostics  read-only runtime telemetry (GET /api/diagnostics).
 *   Tools        bench instruments: SysEx identity/tester, MIDI monitor, the
 *                integrated note tester.
 *
 * The Tools tab owns a live MIDI socket, torn down on every tab switch and on
 * close.
 */
(function (global) {
  'use strict';
  var GMB = global.GMB, h = GMB.h;

  var overlay = null;
  var activeTab = 'profiles';
  // openNetwork/pickedSsid: the "no password needed" state is only trusted while
  // the SSID field still holds the exact network picked from the scan — a manual
  // edit falls back to "unknown security" and shows the password field again.
  var wifi = { stationPassword: '', apPassword: '', forgetStation: false,
               forgetAp: false, openNetwork: false, pickedSsid: '' };
  // Wi-Fi scan state: null until a scan ran; { scanning, networks } afterwards.
  var scan = null;
  var scanPollTimer = null;

  function openNetworkSelected() {
    var net = GMB.state.profile && GMB.state.profile.network;
    return wifi.openNetwork && net && net.ssid === wifi.pickedSsid;
  }

  // Three categories, kept strictly apart, because mixing them is what produced
  // two editors for the network and a profile library nobody could reach:
  //
  //   INSTRUMENT config  -> Profiles (the library; the instrument itself is built
  //                         on the Setup page)
  //   DEVICE config      -> Network, Security  (they stay with the machine)
  //   DIAGNOSTIC tools   -> Diagnostics (read-only telemetry), Tools (SysEx +
  //                         MIDI monitor + note tester)
  var TABS = [
    { id: 'profiles',    label: 'Profiles' },
    { id: 'network',     label: 'Network' },
    { id: 'security',    label: 'Security' },
    { id: 'diagnostics', label: 'Diagnostics' },
    { id: 'tools',       label: 'Tools' }
  ];

  function section(title, children, hint) {
    return h('div.settings-section', [
      h('h3', title), hint ? h('p.muted', hint) : null, children
    ]);
  }

  // ---- shell ----------------------------------------------------------------
  function build() {
    var panel = h('div.settings-panel', { onclick: function (e) { e.stopPropagation(); } }, [
      h('div.settings-head', [
        h('div.settings-head-top', [
          h('h2', 'Settings'),
          h('button.settings-close', { type: 'button', title: 'Close', onclick: close }, '×')
        ]),
        h('div.settings-tabs', TABS.map(function (t) {
          return h('button.settings-tab' + (t.id === activeTab ? '.active' : ''),
            { type: 'button', onclick: function () { switchTab(t.id); } }, t.label);
        }))
      ]),
      h('div.settings-body', { id: 'settings-body' }),
      h('div.settings-actions', [
        h('span.muted', GMB.api.mock ? 'Demo / mock backend' : ''),
        h('span.spacer'),
        GMB.button('Close', close, 'ghost'),
        GMB.button('Save & publish', save, 'primary')
      ])
    ]);

    overlay = h('div.settings-overlay', { onclick: close }, [panel]);
    document.body.appendChild(overlay);
    drawTab();
  }

  function drawTab() {
    var body = document.getElementById('settings-body');
    if (!body) return;
    teardownTab();
    body.innerHTML = '';
    if (activeTab === 'tools') toolsTab(body);
    else if (activeTab === 'security') securityTab(body);
    else if (activeTab === 'diagnostics') diagnosticsTab(body);
    else if (activeTab === 'profiles') profilesTab(body);
    else networkTab(body);
  }

  function switchTab(id) {
    if (id === activeTab) return;
    teardownTab();
    activeTab = id;
    if (overlay) overlay.querySelectorAll('.settings-tab').forEach(function (b, i) {
      b.classList.toggle('active', TABS[i] && TABS[i].id === id);
    });
    drawTab();
  }

  // Release anything live a tab may hold (MIDI socket, running test sequence,
  // Wi-Fi scan polling).
  function teardownTab() {
    if (diagTimer) { clearInterval(diagTimer); diagTimer = null; }
    if (GMB.midiSettings && GMB.midiSettings.teardown) GMB.midiSettings.teardown();
    if (GMB.views.midi && GMB.views.midi.teardown) GMB.views.midi.teardown();
    if (GMB.testRunner && GMB.testRunner.stop) GMB.testRunner.stop();
    stopScanPoll();
  }

  // ---- Network tab ----------------------------------------------------------

  // Poll GET /api/wifi/scan while the survey runs, re-rendering the list as
  // results land. The timer dies with the tab/modal (teardownTab/close).
  function pollScan() {
    GMB.api.wifiScan(false).then(function (r) {
      scan = r;
      renderScanList();
      if (r && r.scanning) scanPollTimer = setTimeout(pollScan, 800);
      else scanPollTimer = null;
    }).catch(function () { scanPollTimer = null; });
  }

  function startScan() {
    scan = { scanning: true, networks: (scan && scan.networks) || [] };
    renderScanList();
    GMB.api.wifiScan(true).then(function (r) {
      scan = r;
      renderScanList();
      if (scanPollTimer) clearTimeout(scanPollTimer);
      scanPollTimer = setTimeout(pollScan, 800);
    }).catch(function (e) {
      scan = null;
      renderScanList();
      GMB.toast('Wi-Fi scan failed: ' + (e && e.message || e), 'error');
    });
  }

  function stopScanPoll() {
    if (scanPollTimer) { clearTimeout(scanPollTimer); scanPollTimer = null; }
  }

  function pickNetwork(entry) {
    var net = GMB.state.profile.network;
    net.mode = 'station';
    net.ssid = entry.ssid;
    wifi.pickedSsid = entry.ssid;
    wifi.openNetwork = !entry.secure;
    if (wifi.openNetwork) wifi.stationPassword = '';  // open: no password to send
    GMB.markDirty();
    drawTab();
  }

  function renderScanList() {
    var box = document.getElementById('wifi-scan-list');
    if (!box) return;
    box.innerHTML = '';
    if (!scan) return;
    if (scan.scanning) box.appendChild(h('p.muted', 'Scanning…'));
    var nets = scan.networks || [];
    if (!scan.scanning && !nets.length)
      box.appendChild(h('p.muted', 'No network found. Scan again, or type the SSID by hand.'));
    nets.forEach(function (n) {
      var row = h('button.btn.ghost.wifi-row', { type: 'button', onclick: function () { pickNetwork(n); } }, [
        h('span', n.ssid),
        h('span.muted', ' ' + (n.secure ? '🔒' : 'open') + ' · ' + n.rssi + ' dBm · ch ' + n.channel)
      ]);
      row.style.display = 'block';
      row.style.width = '100%';
      row.style.textAlign = 'left';
      box.appendChild(row);
    });
  }

  function networkTab(host) {
    var net = GMB.state.profile.network;

    host.appendChild(section('Network', h('div.form-grid', [
      GMB.field('Mode', GMB.input(net, 'mode', {
        type: 'select',
        options: [{ value: 'accessPoint', label: 'Access point (hotspot)' },
                  { value: 'station', label: 'Wi-Fi client' }],
        onChange: drawTab
      })),
      GMB.field('Access-point SSID', GMB.input(net, 'apSsid')),
      net.mode === 'station'
        ? GMB.field('Station SSID', GMB.input(net, 'ssid', {
            // A hand-edited SSID is no longer the scanned (possibly open) network:
            // security becomes unknown again, so the password field comes back.
            onChange: function () {
              if (net.ssid !== wifi.pickedSsid) { wifi.openNetwork = false; drawTab(); }
            }
          }))
        : null,
      GMB.field('Hostname', GMB.input(net, 'hostname'))
    ]), 'Stored on the device itself (not in the instrument profile), so it survives ' +
        'reboots and profile changes. “Save & publish” applies it immediately; if the ' +
        'connection fails the device falls back to its hotspot.'));

    if (net.mode === 'station') {
      var scanKids = [
        h('div.toolbar', [GMB.button('Scan networks', startScan, 'ghost')]),
        h('div', { id: 'wifi-scan-list' })
      ];
      host.appendChild(section('Nearby networks', h('div', scanKids),
        'Pick a network to fill the SSID. Open networks need no password.'));
    }

    var credKids = [];
    if (net.mode === 'station') {
      if (openNetworkSelected()) {
        credKids.push(h('p.muted', '“' + (net.ssid || '') + '” is an open network — no password needed.'));
      } else {
        credKids.push(GMB.field('Station password', GMB.input(wifi, 'stationPassword', { type: 'password' })));
      }
      var forget = h('input', { type: 'checkbox', checked: !!wifi.forgetStation });
      forget.addEventListener('change', function () { wifi.forgetStation = forget.checked; });
      credKids.push(h('label.inline.builder-opt', [forget,
        h('span', 'Forget the stored station password')]));
    }
    if (!wifi.forgetAp) {
      credKids.push(GMB.field('Access-point password', GMB.input(wifi, 'apPassword', { type: 'password' }),
        '8–63 characters (WPA2)'));
    }
    var forgetAp = h('input', { type: 'checkbox', checked: !!wifi.forgetAp });
    forgetAp.addEventListener('change', function () {
      wifi.forgetAp = forgetAp.checked;
      if (wifi.forgetAp) wifi.apPassword = '';
      drawTab();
    });
    credKids.push(h('label.inline.builder-opt', [forgetAp,
      h('span', 'Remove the hotspot password (OPEN access point)')]));
    host.appendChild(section('Wi-Fi credentials', h('div.form-grid', credKids),
      'Write-only — never displayed or exported. Leave blank to keep unchanged; use ' +
      '“Forget” / “Remove” (or pick an open network) to really erase a stored password.'));

    host.appendChild(section('Hotspot', h('div.toolbar', [
      GMB.button('Start hotspot now', startHotspot, 'ghost')
    ]), 'Switch to the access point now with a captive portal: joining the device’s Wi-Fi opens this page. Also available by holding the board BOOT button for ~2 s.'));

    renderScanList();
  }

  // ---- Advanced tab ---------------------------------------------------------
  var adminTok = { token: '', confirm: '', current: '' };

  // Re-read the live status and re-render the security section (policy radios,
  // unlock button, protection banner) so a change is visible IMMEDIATELY.
  function refreshSecurity() {
    var sec = document.getElementById('security-section');
    if (!sec) return;
    GMB.api.getStatus().then(function (st) { renderSecurity(sec, st); })
      .catch(function () {});
  }

  function unlockBrowser() {
    if (!adminTok.current) {
      GMB.toast('Enter the current admin token first.', 'warn');
      return;
    }
    GMB.api.unlockAdminToken(adminTok.current)
      .then(function () {
        adminTok.current = '';
        GMB.toast('Token accepted — this browser is now authorised for writes.', 'ok');
        refreshSecurity();
      })
      .catch(function () {
        GMB.toast('Wrong token — the device refused it.', 'error');
      });
  }

  function setAdminToken() {
    if (!adminTok.token || adminTok.token.length < 8) {
      GMB.toast('Admin token must be at least 8 characters.', 'error');
      return;
    }
    if (adminTok.token !== adminTok.confirm) {
      GMB.toast('The two token fields do not match.', 'error');
      return;
    }
    GMB.api.setAdminTokenRemote(adminTok.token)
      .then(function () {
        adminTok.token = ''; adminTok.confirm = '';
        GMB.toast('Admin token set — write API calls now require it (this browser ' +
                  'remembers it).', 'ok');
        drawTab();
      })
      .catch(function (e) {
        GMB.toast('Token change failed: ' + ((e && e.body && e.body.error) ||
                  (e && e.message) || e), 'error');
      });
  }

  // Device security: admin token workflow + UDP MIDI source posture, rendered from
  // the live status (audit 4 P2.2 / P2.3).
  function renderSecurity(box, st) {
    box.innerHTML = '';
    var configured = st ? !!st.authConfigured : null;
    var adminKids = [
      h('p' + (configured === false ? '.warn-text' : '.muted'),
        configured === null ? 'Protection: …'
          : configured ? 'Protection: configured — write API calls require the admin token.'
                       : 'Protection: NOT configured — anyone reaching this page can ' +
                         'change settings. Set a token before joining a shared network.')
    ];
    if (configured) {
      // A NEW browser that KNOWS the token must be able to authorise itself
      // without changing the device's token (audit 5): verify via
      // /api/auth/check, then remember it locally.
      adminKids.push(GMB.field('Current admin token',
        GMB.input(adminTok, 'current', { type: 'password' }),
        'authorise THIS browser with the existing token'));
      adminKids.push(h('div.toolbar', [GMB.button('Unlock this browser', unlockBrowser, 'primary')]));
    }
    adminKids.push(GMB.field(configured ? 'New admin token' : 'Admin token',
      GMB.input(adminTok, 'token', { type: 'password' }), 'at least 8 characters'));
    adminKids.push(GMB.field('Confirm token', GMB.input(adminTok, 'confirm', { type: 'password' })));
    adminKids.push(h('div.toolbar', [GMB.button(configured ? 'Change token' : 'Set token',
      setAdminToken, configured ? 'ghost' : 'primary')]));
    box.appendChild(section('Admin access', h('div.form-grid', adminKids),
      'Stored on the device (never exported); this browser keeps its copy locally ' +
      'so your own writes keep working.'));

    var policy = (st && st.midiSourcePolicy) || 'open';
    var locked = !!(st && st.midiSourceLocked);
    function policyRadio(value, label, hint) {
      var input = h('input', { type: 'radio', name: 'midisrc', checked: policy === value });
      input.addEventListener('change', function () {
        GMB.api.setMidiSource({ policy: value }).then(function () {
          GMB.toast('Network MIDI source policy: ' + label, 'ok');
          refreshSecurity();  // show the unlock button / lock state right away
        }).catch(function (e) {
          GMB.toast('Policy change failed: ' + ((e && e.message) || e), 'error');
        });
      });
      return h('label.inline.builder-opt', [input, h('span', label + ' — ' + hint)]);
    }
    var midiKids = [
      policyRadio('open', 'Accept any sender', 'any host on the network may send notes'),
      policyRadio('lockToFirst', 'Lock to first sender',
        'the first controller heard becomes the only accepted one' +
        (locked ? ' (currently locked to a sender)' : '')),
      policyRadio('disabled', 'Disable network MIDI', 'refuse every UDP MIDI packet')
    ];
    if (policy === 'lockToFirst') {
      midiKids.push(h('div.toolbar', [GMB.button('Unlock current sender', function () {
        GMB.api.setMidiSource({ unlock: true }).then(function () {
          GMB.toast('Sender unlocked — the next controller heard will lock the session.', 'ok');
          refreshSecurity();
        }).catch(function (e) {
          GMB.toast('Unlock failed: ' + ((e && e.message) || e), 'error');
        });
      }, 'ghost')]));
    }
    box.appendChild(section('MIDI network source', h('div', midiKids),
      'Stored on the device. On the isolated hotspot “accept any” is fine; on a ' +
      'shared Wi-Fi prefer “lock to first sender”.'));

    // Which inputs are actually live. The policy above governs the Wi-Fi transport
    // only, so it is worth being explicit that a DIN cable is not covered by it.
    var transports = (st && st.midiTransports) || [];
    if (transports.length) {
      box.appendChild(section('MIDI inputs', h('table.cap-table.diag-table', [h('tbody',
        transports.map(function (t) {
          return h('tr', [
            h('td', t.label || t.name),
            h('td', h('span.pill' + (t.bound ? '.ok' : ''), t.bound ? 'live' : 'inactive')),
            h('td.muted', (t.detail || '') +
              (t.events ? ' · ' + t.events.toLocaleString() + ' messages' : ''))
          ]);
        }))]),
        'DIN needs a MIDI_RX GPIO (Wiring & GPIO → GPIO pins). The source policy above applies ' +
        'to the Wi-Fi transport only — a physical cable is trusted by being plugged in.'));
    }
  }

  // ---- Diagnostics (GET /api/diagnostics, audit P2.19) ----------------------
  // The numbers a bench session actually needs, in the order you reach for them:
  // is it up and did it reset cleanly, is the control loop keeping its deadlines,
  // is anything being dropped, and is the hardware still all there. The body is
  // built by the firmware's main loop and only COPIED by the web task, so reading
  // this never touches the I2C bus or a live counter.
  var diagTimer = null;
  function fmtUptime(ms) {
    var s = Math.floor((ms || 0) / 1000);
    var d = Math.floor(s / 86400), hh = Math.floor((s % 86400) / 3600);
    var mm = Math.floor((s % 3600) / 60), ss = s % 60;
    return (d ? d + 'd ' : '') + (d || hh ? hh + 'h ' : '') + mm + 'm ' + ss + 's';
  }
  function fmtKb(b) { return b == null ? '—' : Math.round(b / 1024).toLocaleString() + ' KiB'; }
  function statRow(label, value, hint) {
    return h('tr', [h('td', label), h('td', h('strong', String(value))),
                    h('td.muted', hint || '')]);
  }
  function diagnosticsBody(d) {
    if (!d) return h('p.muted', 'Reading diagnostics…');
    var sc = d.scheduler || {}, mi = d.midi || {}, mo = d.motion || {},
        mx = d.moveMix || {}, pca = d.pca || {};
    var rows = [
      statRow('Uptime', fmtUptime(d.uptimeMs), 'since the last reset'),
      statRow('Last reset', d.resetReason || '—',
        'powerOn is a clean start; brownout / wdt / panic each point somewhere specific'),
      statRow('State', d.state || '—', 'the application phase'),
      statRow('Free heap', fmtKb(d.freeHeap), 'minimum seen: ' + fmtKb(d.minFreeHeap)),
      statRow('Loop period (mean)', (sc.meanUs || 0) + ' µs',
        'every musical deadline lives inside this'),
      statRow('Loop worst case', (sc.maxLatencyUs || 0) + ' µs',
        'jitter ' + (sc.jitterUs || 0) + ' µs — a spike here is a late note'),
      statRow('Command queue high-water', d.cmdQueueHighWater || 0,
        'deepest the web→loop queue ever got'),
      statRow('MIDI events', mi.events || 0,
        (mi.droppedEvents || 0) + ' dropped · ' + (mi.droppedPackets || 0) +
        ' datagrams dropped · ' + (mi.rejectedPackets || 0) + ' refused by the source gate'),
      statRow('Faults', d.faults || 0, 'cumulative, survives clearing the visible log'),
      statRow('Carriage moves', mo.axisMoves || 0,
        (mo.homingFailures || 0) + ' homing failure(s) · ' + (mo.limitTrips || 0) +
        ' LIMIT trip(s) · ' + (mo.moveTimeouts || 0) + ' move timeout(s)'),
      statRow('Servo pulses', d.servoMoves || 0, ''),
      statRow('Move mix', (mx.deadline || 0) + ' deadline / ' + (mx.staggerableGranted || 0) +
        ' staggerable',
        (mx.staggerableDeferred || 0) + ' deferred by the start governor — deadline moves ' +
        '(the sound) are never throttled'),
      statRow('Wi-Fi reconnects', d.wifiReconnects || 0, ''),
      statRow('PCA9685', pca.used ? (pca.healthy ? 'all responding' : 'FAULT') : 'not used',
        pca.failedBoard ? 'silent board: ' + pca.failedBoard : '')
    ];
    return h('table.cap-table.diag-table', [h('tbody', rows)]);
  }
  function diagnosticsTab(host) {
    host.appendChild(h('div.note-box',
      'Runtime telemetry straight from the device (GET /api/diagnostics). Nothing here ' +
      'is configuration — it is what the firmware has observed since it booted, which is ' +
      'what a bench session needs when something behaves oddly but nothing has faulted.'));
    var box = h('div.card', [h('div.card-head', [h('h2', 'Runtime telemetry'),
      h('span.muted', 'refreshes every 2 s while this tab is open')]),
      h('div#diag-body', diagnosticsBody(null))]);
    host.appendChild(box);

    function refresh() {
      GMB.api.getDiagnostics().then(function (d) {
        var slot = document.getElementById('diag-body');
        if (!slot) return;                       // the panel closed meanwhile
        slot.innerHTML = '';
        slot.appendChild(diagnosticsBody(d));
      }).catch(function () {});
    }
    refresh();
    if (diagTimer) clearInterval(diagTimer);
    diagTimer = setInterval(function () {
      if (!document.getElementById('diag-body')) { clearInterval(diagTimer); diagTimer = null; return; }
      refresh();
    }, 2000);
  }

  // ---- Profiles tab ---------------------------------------------------------
  // profiles.js has always been complete — slots, save, copy, rename, delete,
  // import/export, restore — and until now nothing navigated to it,
  // so a whole working feature was invisible. It belongs here: the instrument
  // LIBRARY is not part of building one instrument (that is the Setup page), and
  // it is not a device setting either.
  function profilesTab(host) {
    host.appendChild(h('div.note-box',
      'Saved instruments — a LIBRARY. What the device boots is whatever is ' +
      'currently running, so publishing or loading a profile is what changes it; ' +
      'there is no separate “startup” choice to keep in sync. The network settings ' +
      'and Wi-Fi passwords are NOT part of a profile — they stay with the machine.'));
    if (GMB.views.profiles && GMB.views.profiles.render) GMB.views.profiles.render(host);
  }

  // ---- Security tab (device) ------------------------------------------------
  function securityTab(host) {
    host.appendChild(h('div.note-box',
      'Who may change this device, and who may play it over the network. Both ' +
      'are stored on the device and are never part of an exported profile.'));
    var sec = h('div', { id: 'security-section' });
    host.appendChild(sec);
    renderSecurity(sec, null);
    GMB.api.getStatus().then(function (st) { renderSecurity(sec, st); })
      .catch(function () {});
  }

  // ---- Tools tab (diagnostics, not configuration) ---------------------------
  function toolsTab(host) {
    host.appendChild(h('div.note-box',
      'Bench tools: what the instrument announces over SysEx and a tester for it, ' +
      'the live MIDI monitor, and the integrated note tester that walks the real ' +
      'string/fret selection chain.'));
    if (GMB.views.sysex && GMB.views.sysex.render) GMB.views.sysex.render(host);
    if (GMB.midiSettings && GMB.midiSettings.tools) GMB.midiSettings.tools(host);
  }

  // ---- open / close ---------------------------------------------------------
  function open(tab) {
    if (!GMB.state.profile) { GMB.toast('Configuration still loading…', 'warn'); return; }
    activeTab = (tab && TABS.some(function (t) { return t.id === tab; })) ? tab : 'profiles';
    if (overlay) overlay.remove();
    build();
    // rAF so the .open transition runs from the hidden state.
    requestAnimationFrame(function () { if (overlay) overlay.classList.add('open'); });
    document.addEventListener('keydown', onKey);
  }
  GMB.openSettings = open;

  function close() {
    document.removeEventListener('keydown', onKey);
    teardownTab();
    if (overlay) {
      var o = overlay;
      o.classList.remove('open');
      overlay = null;
      setTimeout(function () { o.remove(); }, 200);
    }
    // Refresh the underlying page so config changes show, and reset the wizard's
    // current-flow tracking to whatever page is now visible.
    if (GMB.render) GMB.render();
  }
  GMB.closeSettings = close;

  function onKey(e) { if (e.key === 'Escape') close(); }

  // ---- save -----------------------------------------------------------------
  // Order matters (audit 4 P1.6): the PROFILE is published FIRST, over the link we
  // still have; the network settings (device NVS) go second; and only then is the
  // Wi-Fi change APPLIED — apply:true may tear down the very connection the
  // browser is using (hotspot -> station or back), which used to kill the profile
  // PUT that was still queued behind it.
  function save() {
    var net = GMB.state.profile.network;
    // WPA2 needs 8..63 chars — anything shorter would silently start an OPEN
    // hotspot, so refuse it here too (the API also answers 422).
    if (wifi.apPassword && (wifi.apPassword.length < 8 || wifi.apPassword.length > 63)) {
      GMB.toast('Hotspot password must be 8–63 characters (WPA2) — or use ' +
                '“Remove the hotspot password” for an open access point.', 'error');
      return;
    }
    var payload = {
      mode: net.mode,
      ssid: net.ssid || '',
      apSsid: net.apSsid || '',
      hostname: net.hostname || '',
      apply: true  // the device reconnects only after everything is stored
    };
    if (wifi.stationPassword) payload.stationPassword = wifi.stationPassword;
    if (wifi.apPassword) payload.apPassword = wifi.apPassword;
    if (wifi.forgetStation || (openNetworkSelected() && !wifi.stationPassword))
      payload.clearStationPassword = true;
    if (wifi.forgetAp) payload.clearApPassword = true;
    var switching = net.mode === 'station';
    GMB.saveProfile()
      .then(function () {
        // Profile safely published: NOW store + apply the network settings.
        return GMB.api.setWifi(payload).then(function (r) {
          wifi.stationPassword = ''; wifi.apPassword = '';
          wifi.forgetStation = false; wifi.forgetAp = false;
          // Report what the DEVICE says happened, not what we hoped: `applied`
          // comes back false on a build that can only store the settings.
          GMB.toast(r && r.applied
            ? 'Network settings applied — the device is reconfiguring its radio.'
            : ('Network settings ' + ((r && r.note) || 'stored — reboot to apply.')),
            'ok');
          if (switching && r && r.applied)
            GMB.toast('If you are connected through the hotspot, the device may now ' +
                      'switch networks — reconnect on the new network if this page ' +
                      'stops responding.', 'warn');
        }, function (e) {
          var body = e && e.body;
          GMB.toast('Network save failed: ' + ((body && body.error) || (e && e.message) || e),
                    'error');
        });
      })
      .catch(function () {
        // Profile save failed (already toasted): the network change was NOT
        // stored or applied — fix the draft and save again.
      });
  }

  function startHotspot() {
    if (!confirm('Switch to the Wi-Fi hotspot (access point) now?\n\nIf you are connected over Wi-Fi you will be disconnected — rejoin the device’s network (the config page opens automatically).')) return;
    GMB.api.startHotspot().then(function (r) {
      GMB.toast((r && r.note) || 'Hotspot starting…', 'warn');
    }).catch(function (e) { GMB.toast('Hotspot request failed: ' + (e && e.message || e), 'error'); });
  }

  GMB.views = GMB.views || {};
})(window);
