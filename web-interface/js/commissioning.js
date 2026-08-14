/*
 * commissioning.js — "Commissioning" sub-tab of the Wiring & GPIO page.
 *
 * The staged electrical acceptance procedure of hardware/COMMISSIONING.md as a
 * live checklist: each stage has a gate — do not power the next stage until
 * every box of the current one holds. Progress is kept in localStorage per
 * instrument (bench state, not configuration), so it survives reloads without
 * touching the profile. The full procedure, with the measurements to record,
 * stays in hardware/COMMISSIONING.md.
 */
(function (global) {
  'use strict';
  var GMB = global.GMB, h = GMB.h;

  var STAGES = [
    { id: 's0', title: 'Stage 0 — visual & mechanical (all supplies OFF)', items: [
      'Every servo lead, motor lead, power branch and the I²C harness routed and strain-relieved; connectors latched',
      'PCA9685 A0–A2 jumpers match the I²C & PCA tab (bus + address per board)',
      'Each carriage slides FREELY by hand over its whole travel — no binding, no cable snag at either end',
      'HOME and LIMIT endstops mounted at OPPOSITE ends of each axis, and actually triggered by the carriage',
      'Belt tension / lead-screw backlash set; pulleys and couplers tight on their shafts',
      'Branch fuses F1…Fn and the main fuse F0 installed at their rated values',
      'E-stop button mounted, reachable, latched released',
      'No string under tension yet'
    ] },
    { id: 's1', title: 'Stage 1 — continuity & shorts (all supplies OFF)', items: [
      'All grounds (ESP32, PCA boards, PSU, distribution block) are one net',
      'No continuity +V_SERVO↔GND, 3V3↔GND, +V_SERVO↔3V3',
      'E-stop chain: every NC contact closed when released, open when pressed',
      '/OE bus continuous to every board’s /OE pin; pull-up to 3.3 V present',
      'Driver ENABLE line continuous to every driver; pull-up so the drivers are OFF with the ESP32 absent',
      'Each driver’s STEP/DIR pair goes to the axis it is labelled for (swapped pairs are the classic first-run fault)',
      'HOME/LIMIT inputs read the expected level released, and the opposite level when the endstop is pressed by hand'
    ] },
    { id: 's2', title: 'Stage 2 — logic only (USB in, servo PSU OFF)', items: [
      'ESP32-S3 boots, web UI reachable, profile loaded',
      'GPIO validation shows no errors (SDA/SCL per used bus, /OE, ESTOP)',
      'Profile loaded but NOT armed → /OE bus measures HIGH (≈3.3 V) and ENABLE is inactive (drivers de-energised)',
      'E-stop press shows EmergencyStop in the UI; release + Reset clears it',
      'NC wiring: unplugging the chain connector also reads STOP (fail-safe check)',
      'Every configured PCA answers at its (bus, address) on an arming attempt',
      'Every enabled axis reports a step generator attached (no attach fault at boot)'
    ] },
    { id: 's3', title: 'Stage 3 — servo + motor rails on, outputs disabled (do not arm)', items: [
      'Servo rail voltage correct at the distribution block and at EACH branch',
      'Motor rail voltage correct at EACH driver’s supply terminals',
      'Every driver’s Vref set to its motor’s rated phase current — MEASURE it, do not trust the pot position',
      '/OE still HIGH and ENABLE still inactive — no servo twitch, no motor holding torque at power-on',
      'Idle current plausible (boards’ + drivers’ quiescent draw only; the motors must be COLD)',
      'E-stop press drops the contactor: 0 V on every branch, both rails'
    ] },
    { id: 's4', title: 'Stage 4 — first motion, one axis (strings off)', items: [
      'Arm from the UI: the fingers lift FIRST (governed park), then the carriages seek HOME → Ready',
      'Each axis moves TOWARD its HOME sensor on the seek — a run to the far end means DIR is inverted; stop and fix invertDirection before anything else',
      'HOME found, back-off + offset applied, position reads ~0 mm at the reference',
      'Jog one axis a known distance (Instrument page) and MEASURE it: mm on screen = mm on the machine, or the steps/mm is wrong',
      'One finger servo moves correctly (direction, travel, rest)',
      'E-stop during a carriage move stops it instantly; Reset + re-home works after',
      'Same E-stop test on a direct-GPIO servo, if any (only the power cut stops it)'
    ] },
    { id: 's5', title: 'Stage 5 — per-axis / per-branch bring-up', items: [
      'Every servo of each branch reaches rest and press/pluck positions',
      'Every axis reaches fret 0 and its highest fret without hitting a soft limit or a LIMIT endstop',
      'Fret positions checked against the fretboard: adjust fretOffsetMm per string, then calibrate individual frets if needed',
      'Repeat a move to the same fret 20× and confirm the carriage returns to the same place (no lost steps)',
      'Branch current at worst realistic case within its fuse’s continuous rating',
      'Motor rail sag during a simultaneous multi-axis acceleration < ~5% (else: bigger cap, thicker wiring, lower concurrent-start cap)'
    ] },
    { id: 's6', title: 'Stage 6 — whole instrument', items: [
      'Full-instrument stress pattern: PSU current + sag within spec, nothing overheating',
      'Wi-Fi loss during play releases the notes, instrument stays armed',
      'One PCA unplugged during play degrades only its strings (readyDegraded)',
      'Triggering a LIMIT endstop during play takes ONLY that axis out of service (readyDegraded), the rest keep playing',
      'Driver temperature after a sustained passage is within the heatsink’s comfort — the holding current runs the whole time the instrument is armed',
      'E-stop at full load: instant, complete stop; recovery = release + reset + re-arm',
      'Only now: string the instrument and repeat the motion checks at low velocity'
    ] }
  ];

  function storeKey() {
    var p = GMB.state.profile;
    var name = (p && p.instrument && p.instrument.name) || 'instrument';
    return 'gmb.commissioning.' + (GMB.slug ? GMB.slug(name) : name);
  }
  function load() {
    try { return JSON.parse(localStorage.getItem(storeKey())) || {}; }
    catch (e) { return {}; }
  }
  function save(state) {
    try { localStorage.setItem(storeKey(), JSON.stringify(state)); } catch (e) {}
  }

  function render(host) {
    var state = load();
    var total = 0, done = 0;
    STAGES.forEach(function (st) {
      st.items.forEach(function (_, i) { total++; if (state[st.id + ':' + i]) done++; });
    });

    var pct = total ? Math.round(done / total * 100) : 0;
    host.appendChild(h('div.card', [
      h('div.card-head', [h('h2', 'Commissioning'),
        h('span.muted', 'staged power-up — full procedure: hardware/COMMISSIONING.md')]),
      h('p.muted', 'Each stage is a GATE: do not pass it until every check holds. Progress is stored in this ' +
        'browser per instrument (bench state, not part of the profile). Record the measured currents, sags and ' +
        'fitted values with the machine as the doc asks.'),
      h('div.cap-line', [
        h('span.cap-label', 'Progress'),
        h('div.cap-bar', h('span', { style: 'width:' + pct + '%' })),
        h('span.cap-val', done + ' / ' + total)
      ]),
      h('div.row', [GMB.button('Reset checklist', function () {
        if (!global.confirm('Clear the commissioning progress for this instrument?')) return;
        save({}); GMB.render();
      }, 'ghost')])
    ]));

    var reached = true;   // stages after an incomplete one are truly gated
    STAGES.forEach(function (st) {
      var stDone = st.items.every(function (_, i) { return !!state[st.id + ':' + i]; });
      // A gate really gates (audit P2): a blocked stage's boxes are DISABLED, not
      // just dimmed — out-of-order ticking needs the explicit per-stage override.
      var overridden = !!state['override:' + st.id];
      var unlocked = reached || overridden;
      var status = stDone ? h('span.pill.mini.ok', 'gate passed')
        : (reached ? h('span.pill.mini.warn', 'in progress')
           : (overridden ? h('span.pill.mini.warn', 'unlocked out of order')
                         : h('span.muted', 'blocked by the previous gate')));
      var kids = [h('div.card-head', [h('h3', st.title), status])];
      st.items.forEach(function (item, i) {
        var key = st.id + ':' + i;
        var cb = h('input', { type: 'checkbox', checked: !!state[key], disabled: !unlocked });
        cb.addEventListener('change', function () {
          state[key] = cb.checked; save(state); GMB.render();
        });
        kids.push(h('label.inline.ps-decl', [cb, h('span', item)]));
      });
      if (!unlocked) {
        kids.push(h('div.row', [
          GMB.button('Unlock this stage anyway', function () {
            state['override:' + st.id] = true; save(state); GMB.render();
          }, 'ghost'),
          h('span.muted', 'Only for a deliberate out-of-order check — the previous gate is not passed.')
        ]));
      }
      host.appendChild(h('div.card' + (unlocked ? '' : '.commissioning-blocked'), kids));
      if (!stDone) reached = false;
    });
  }

  GMB.views.commissioning = { render: render };
})(window);
