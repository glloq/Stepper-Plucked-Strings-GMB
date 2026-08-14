/*
 * hardware.js — combined "Wiring & GPIO" page (UI redesign).
 *
 * One of the three main pages a builder needs. It hosts, behind sub-tabs, the
 * hardware views:
 *   • Harness        — the adaptive ESP32 + PCA9685 harness diagram (wiring.js)
 *   • Power & safety — power tree, E-stop / /OE chain status, current estimator
 *                      (wiringpower.js — hardware/POWER_AND_SAFETY.md live)
 *   • I²C & PCA      — buses, addresses/jumpers, pull-up equivalents (wiringi2c.js)
 *   • GPIO pins      — the GPIO pin-assignment grid + validation (pins.js)
 *   • Commissioning  — the staged power-up checklist (commissioning.js)
 * It simply delegates to GMB.views.*, so all the logic stays in those modules;
 * this file only owns the sub-tab switch.
 */
(function (global) {
  'use strict';
  var GMB = global.GMB, h = GMB.h;

  var sub = 'wiring';   // 'wiring' | 'power' | 'i2c' | 'gpio' | 'commissioning'

  function subBtn(id, label) {
    return h('button.subtab' + (sub === id ? '.active' : ''),
      { type: 'button', onclick: function () { sub = id; GMB.render(); } }, label);
  }

  function render(host) {
    host.appendChild(h('div.card.hw-head', [
      h('div.card-head', [h('h2', 'Wiring & GPIO'),
        h('span.muted', 'harness, power & safety, I²C, pins, commissioning')]),
      h('div.subtabs', [
        subBtn('wiring', 'Harness'),
        subBtn('power', 'Power & safety'),
        subBtn('i2c', 'I²C & PCA'),
        subBtn('gpio', 'GPIO pins'),
        subBtn('commissioning', 'Commissioning')
      ])
    ]));

    // Each sub-view renders into its own section (a fresh element every switch).
    var section = h('div.hw-section');
    host.appendChild(section);
    if (sub === 'gpio') GMB.views.pins.render(section);
    else if (sub === 'power') GMB.views.wiringPower.render(section);
    else if (sub === 'i2c') GMB.views.wiringI2c.render(section);
    else if (sub === 'commissioning') GMB.views.commissioning.render(section);
    else GMB.views.wiring.render(section);
  }

  GMB.views.hardware = { render: render };
})(window);
