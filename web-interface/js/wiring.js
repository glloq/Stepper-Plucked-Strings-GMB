/*
 * wiring.js — graphical wiring map ("Wiring" tab).
 *
 * Draws the electrical harness of the CURRENT instrument as close to the real
 * build as the profile allows, and fully adaptive: everything is derived from
 * GMB.state.profile (the same draft the wizard edits), so the picture updates
 * with every mechanical / pin / servo choice made during instrument creation.
 *
 * What it shows (SPECIFICATION.md §7 / §11 / §22, hardware/wiring/WIRING.md):
 *   • the ESP32-S3 module with its board-level signals — I²C SDA/SCL (and the
 *     optional second bus), the PCA9685 /OE safety line and the shared stepper
 *     driver ENABLE — read from profile.pins;
 *   • one STEP/DIR stepper driver per axis, with its STEP / DIR / HOME / LIMIT
 *     GPIOs and the motor rail that feeds it — the carriage side of this machine,
 *     which the servo-only build has no equivalent of;
 *   • a separate 5–6 V servo PSU and a separate motor PSU (never the ESP
 *     regulator) feeding their rails;
 *   • one PCA9685 breakout per distinct pcaBoard actually used, at its real I²C
 *     address (0x40 + index, set by the A0–A2 jumpers), with its 16 channels laid
 *     out and every occupied channel labelled with the servo it drives
 *     (string / fret / plucker / strum / lift / damper / aux);
 *   • the shared I²C bus + /OE + power rails as horizontal buses every board taps;
 *   • any direct-GPIO servos wired straight to an ESP32 output pin.
 *
 * It also flags real wiring faults live: a duplicated board+channel, two servos
 * (or a servo and a board signal) on the same GPIO, an unassigned SDA/SCL/OE
 * while a PCA is in use, and the firmware capacity limits (8 boards, 8 direct
 * servos). Read-only: it drives no hardware, so there is nothing to arm or tear
 * down. A "Download SVG" button saves the diagram to take to the workbench.
 */
(function (global) {
  'use strict';
  var GMB = global.GMB, h = GMB.h;
  var SVGNS = 'http://www.w3.org/2000/svg';

  // ---- servo function metadata (colour class + short code + long name) ------
  // fn-* classes map to the --wire-* fill tokens in style.css so both themes work.
  var FN = {
    finger:    { cls: 'fn-finger', name: 'Finger' },
    pluck:     { cls: 'fn-pluck',  name: 'Plucker' },
    strum:     { cls: 'fn-strum',  name: 'Strum' },
    strumLift: { cls: 'fn-lift',   name: 'Strum lift' },
    damper:    { cls: 'fn-damper', name: 'Damper' },
    aux:       { cls: 'fn-aux',    name: 'Auxiliary' }
  };

  // ---- small SVG builder (local, mirrors fretboard.js) ----------------------
  function svg(tag, attrs, kids) {
    var el = document.createElementNS(SVGNS, tag);
    if (attrs) Object.keys(attrs).forEach(function (k) {
      var v = attrs[k];
      if (v === null || v === undefined || v === false) return;
      if (k === 'text') el.textContent = v;
      else el.setAttribute(k, v);
    });
    appendKids(el, kids);
    return el;
  }
  function appendKids(el, kids) {
    if (kids === null || kids === undefined) return;
    if (Array.isArray(kids)) { kids.forEach(function (k) { appendKids(el, k); }); return; }
    if (kids.nodeType) { el.appendChild(kids); return; }
    el.appendChild(document.createTextNode(String(kids)));
  }
  function hex2(n) { return '0x' + n.toString(16).toUpperCase(); }

  // ---- profile → wiring model -----------------------------------------------

  function signalGpio(p, name) {
    var a = (p.pins || []).filter(function (x) { return x.signal === name; })[0];
    return a && a.gpio >= 0 ? a.gpio : -1;
  }

  // The tag inside a channel pad: which string the servo belongs to, so a
  // PCA9685 shared across several strings stays unambiguous. Global aux = "GLB".
  function stringTag(sv) { return sv.stringIndex >= 0 ? 'S' + (sv.stringIndex + 1) : 'GLB'; }
  // The strings a board hosts (board-level "shared across strings" readout).
  function hostedStrings(m, key) {
    var set = {}, aux = false;
    m.byBoard[key].forEach(function (l) {
      l.forEach(function (s) { if (s.stringIndex >= 0) set[s.stringIndex] = 1; else aux = true; });
    });
    var ks = Object.keys(set).map(Number).sort(function (a, b) { return a - b; });
    if (!ks.length) return aux ? 'Aux only' : '—';
    return (ks.length > 1 ? 'Str ' : 'String ') + ks.map(function (i) { return i + 1; }).join(', ') + (aux ? ' +aux' : '');
  }
  // A full human label for a servo (channel tooltip, direct-servo row, summary).
  function servoLabel(sv, auxIndex) {
    var s = sv.stringIndex >= 0 ? 'String ' + (sv.stringIndex + 1) : '';
    switch (sv.function) {
      case 'finger': return s + ' · finger';
      case 'pluck': return s + ' · plucker';
      case 'strum': return s + ' · strum';
      case 'strumLift': return s + ' · strum lift';
      case 'damper': return s + ' · damper';
      case 'aux': return 'Auxiliary #' + (auxIndex + 1);
      default: return s + ' · ' + sv.function;
    }
  }

  // A board is a physical chip identified by (I2C bus, address index). The ESP32-S3
  // has two hardware I2C controllers, so boards can be split across two buses to cut
  // the bus load and refresh faster on large instruments.
  function busOf(s) { return s.i2cBus === 1 ? 1 : 0; }
  function boardKey(bus, board) { return bus + ':' + board; }

  // Build the adaptive model the diagram renders from.
  function buildModel(p) {
    var servos = (p.servos || []);
    var pca = servos.filter(function (s) { return s.source === 'pca'; });
    var direct = servos.filter(function (s) { return s.source === 'gpio'; });

    // Distinct (bus, address) chips, ordered by bus then address.
    var boards = [], seen = {};
    pca.forEach(function (s) {
      var b = { bus: busOf(s), board: s.pcaBoard | 0 };
      b.key = boardKey(b.bus, b.board);
      if (!seen[b.key]) { seen[b.key] = 1; boards.push(b); }
    });
    boards.sort(function (a, b) { return a.bus - b.bus || a.board - b.board; });

    // Channel occupancy per chip: byBoard[key][ch] = [servos…] (dup ⇒ length>1).
    var byBoard = {};
    boards.forEach(function (b) { byBoard[b.key] = []; for (var c = 0; c < 16; c++) byBoard[b.key][c] = []; });
    pca.forEach(function (s) {
      var k = boardKey(busOf(s), s.pcaBoard | 0), c = s.channel | 0;
      if (byBoard[k] && c >= 0 && c < 16) byBoard[k][c].push(s);
    });

    // GPIO occupancy: both buses' board signals + direct servos. Value = labels.
    var byGpio = {};
    var auxSeen = 0;
    function claim(gpio, label) { if (gpio < 0) return; (byGpio[gpio] = byGpio[gpio] || []).push(label); }
    ['SDA', 'SCL', 'SDA2', 'SCL2', 'SERVO_OE', 'SERVO_OE2'].forEach(function (sig) { claim(signalGpio(p, sig), sig); });
    // Aux servos need a stable index for their label; number them in profile order.
    var auxOrder = {};
    servos.forEach(function (s) { if (s.function === 'aux') auxOrder[servos.indexOf(s)] = auxSeen++; });
    direct.forEach(function (s) {
      claim(s.gpio, servoLabel(s, auxOrder[servos.indexOf(s)] || 0));
    });

    // ---- stepper side: one STEP/DIR driver per ENABLED axis -----------------
    // A carriage is what chooses the fret here, so the drivers are as much a part
    // of the harness as the PCA boards. Their pins are per-axis (STEP1/DIR1/…),
    // plus one shared ENABLE for every driver.
    var axes = (p.strings || []).map(function (st, i) {
      var n = String(i + 1);
      return {
        index: i,
        enabled: st.enabled !== false,
        step: signalGpio(p, 'STEP' + n),
        dir: signalGpio(p, 'DIR' + n),
        home: signalGpio(p, 'HOME' + n),
        limit: signalGpio(p, 'LIMIT' + n)
      };
    });
    axes.forEach(function (a) {
      if (!a.enabled) return;   // a disabled axis legitimately claims no pin
      claim(a.step, 'STEP' + (a.index + 1));
      claim(a.dir, 'DIR' + (a.index + 1));
      claim(a.home, 'HOME' + (a.index + 1));
      claim(a.limit, 'LIMIT' + (a.index + 1));
    });
    claim(signalGpio(p, 'ENABLE'), 'ENABLE');
    claim(signalGpio(p, 'ESTOP'), 'ESTOP');

    var useBus1 = boards.some(function (b) { return b.bus === 1; });
    var hasOe2 = (p.pins || []).some(function (x) { return x.signal === 'SERVO_OE2'; });
    return {
      servos: servos, pca: pca, direct: direct, boards: boards,
      axes: axes,
      activeAxes: axes.filter(function (a) { return a.enabled; }),
      enable: signalGpio(p, 'ENABLE'),
      estop: signalGpio(p, 'ESTOP'),
      estopNc: !!(p.board && p.board.estopNormallyClosed),
      byBoard: byBoard, byGpio: byGpio, auxOrder: auxOrder,
      sda: signalGpio(p, 'SDA'), scl: signalGpio(p, 'SCL'),
      sda2: signalGpio(p, 'SDA2'), scl2: signalGpio(p, 'SCL2'),
      oe: signalGpio(p, 'SERVO_OE'), oe2: signalGpio(p, 'SERVO_OE2'),
      usePca: boards.length > 0,
      useBus0: boards.some(function (b) { return b.bus === 0; }),
      useBus1: useBus1,
      // A separate /OE per bus is active when a second-bus /OE (OE2) is configured.
      splitOe: useBus1 && hasOe2
    };
  }

  // Collect blocking / advisory wiring issues from the model (adaptive checks).
  function findIssues(p, m) {
    var out = [];
    // Duplicate channel on a chip (bus + address).
    m.boards.forEach(function (b) {
      m.byBoard[b.key].forEach(function (list, ch) {
        if (list.length > 1) out.push({ sev: 'error',
          msg: 'PCA bus ' + b.bus + ' ' + hex2(0x40 + b.board) + ' channel ' + ch + ' is wired to ' + list.length +
            ' servos (' + list.map(function (s) { return servoLabel(s, m.auxOrder[m.servos.indexOf(s)] || 0); }).join(', ') + ').' });
      });
    });
    // Duplicate GPIO (two servos, or a servo and a board signal).
    Object.keys(m.byGpio).forEach(function (g) {
      var who = m.byGpio[g];
      if (who.length > 1) out.push({ sev: 'error',
        msg: 'GPIO' + g + ' is shared by ' + who.join(' and ') + ' — one pin cannot drive two.' });
    });
    // Missing bus signals — only for a bus that actually carries a board.
    var useBus0 = m.boards.some(function (b) { return b.bus === 0; });
    if (useBus0) {
      if (m.sda < 0) out.push({ sev: 'error', msg: 'I²C bus 0 SDA is unassigned but a PCA9685 is on the primary bus — assign it in the GPIO sub-tab.' });
      if (m.scl < 0) out.push({ sev: 'error', msg: 'I²C bus 0 SCL is unassigned but a PCA9685 is on the primary bus — assign it in the GPIO sub-tab.' });
    }
    if (m.useBus1) {
      if (m.sda2 < 0) out.push({ sev: 'error', msg: 'I²C bus 1 SDA (SDA2) is unassigned but a board is on bus 1 — assign it in the GPIO sub-tab.' });
      if (m.scl2 < 0) out.push({ sev: 'error', msg: 'I²C bus 1 SCL (SCL2) is unassigned but a board is on bus 1 — assign it in the GPIO sub-tab.' });
    }
    // The shared /OE covers a board unless it is a bus-1 board with a split /OE2.
    // Missing /OE is an ERROR, matching the firmware ProfileValidator: a profile
    // with a PCA9685 but no /OE safety line is refused activation, so the page
    // must not soften that to a mere warning.
    var needSharedOe = useBus0 || (m.useBus1 && !m.splitOe);
    if (needSharedOe && m.oe < 0) out.push({ sev: 'error', msg: 'PCA9685 /OE is unassigned — the outputs cannot be force-disabled and the profile cannot be armed. Assign SERVO_OE in the GPIO sub-tab.' });
    if (m.splitOe && m.oe2 < 0) out.push({ sev: 'error', msg: 'The second-bus /OE (OE2) is unassigned — bus 1 outputs cannot be force-disabled and the profile cannot be armed. Assign SERVO_OE2 (or share the single /OE line).' });
    // Hardware E-stop chain (advisory): /OE and the software stop cover the PCA
    // outputs, but only a physical E-stop cutting the servo rail also covers
    // direct-GPIO servos and firmware faults. Declared in the GPIO sub-tab.
    var estop = (p.pins || []).filter(function (x) { return x.signal === 'ESTOP'; })[0];
    if (!estop) out.push({ sev: 'warning',
      msg: 'No hardware E-stop is declared — a software stop is not a safety function. ' +
        'Declare it in the GPIO sub-tab (Emergency stop input); reference circuit in hardware/POWER_AND_SAFETY.md.' });
    else if (estop.gpio < 0) out.push({ sev: 'warning',
      msg: 'The hardware E-stop is declared but its ESTOP input has no GPIO — assign one in the GPIO sub-tab.' });
    // ---- stepper drivers ----------------------------------------------------
    // Every enabled axis needs its three mandatory lines; the firmware refuses to
    // activate a profile that is missing one, so these are errors, not advice.
    m.activeAxes.forEach(function (a) {
      var n = a.index + 1;
      if (a.step < 0) out.push({ sev: 'error', msg: 'Axis ' + n + ': STEP' + n + ' is unassigned — the carriage cannot be driven.' });
      if (a.dir < 0) out.push({ sev: 'error', msg: 'Axis ' + n + ': DIR' + n + ' is unassigned — the carriage has no direction line.' });
      if (a.home < 0) out.push({ sev: 'error', msg: 'Axis ' + n + ': HOME' + n + ' is unassigned — the axis can never be homed, so it can never play.' });
      if (a.limit < 0) out.push({ sev: 'warning', msg: 'Axis ' + n + ': no LIMIT' + n + ' endstop — a missed HOME sensor will only be caught by the search-distance timeout, after the carriage has run to the end of its travel.' });
    });
    if (m.activeAxes.length && m.enable < 0)
      out.push({ sev: 'error', msg: 'The shared driver ENABLE line is unassigned — the drivers cannot be de-energised and the profile cannot be armed.' });
    if (m.activeAxes.length && !m.estopNc && m.estop >= 0)
      out.push({ sev: 'warning', msg: 'The E-stop is wired as a normally-OPEN button: a cut wire or an unplugged connector reads "running". A normally-CLOSED loop fails safe — set it on the GPIO sub-tab and rewire per hardware/POWER_AND_SAFETY.md.' });

    // Firmware capacity limits (max 8 addresses per bus).
    [0, 1].forEach(function (bus) {
      var n = m.boards.filter(function (b) { return b.bus === bus; }).length;
      if (n > 8) out.push({ sev: 'error', msg: n + ' PCA9685 boards on I²C bus ' + bus + ' — at most 8 per bus (0x40–0x47).' });
    });
    if (m.direct.length > 8) out.push({ sev: 'error', msg: m.direct.length + ' direct-GPIO servos — at most 8 (one LEDC channel each).' });
    return out;
  }

  // ---- geometry -------------------------------------------------------------
  var BOARD_W = 200, BOARD_GAP = 26;
  var BOARD_H = 250;
  var ESP_X = 24, TOP_Y = 14, PSU_W = 132;
  // Uniform vertical-tap pitch. The ESP/PSU (sources) tap on the grid; every board
  // (load) taps a HALF-PITCH off it, so an ESP lead and a board lead never share an
  // x — they interleave — while the whole thing stays compact (boards under the ESP).
  var PITCH = 18, TAP_PAD = 18;
  function espTapX(i) { return ESP_X + TAP_PAD + i * PITCH; }
  function boardTapX(bx, k) { return bx + TAP_PAD + PITCH / 2 + k * PITCH; }
  function snapTapX(x) { return ESP_X + TAP_PAD + Math.round((x - ESP_X - TAP_PAD) / PITCH) * PITCH; }
  // Buses sit in a band from BUS_TOP; boards start a clear BAND_GAP below the band
  // so the taps are visible and never overlap the boards.
  var BUS_TOP = 116, BUS_STEP = 13, BAND_GAP = 44;
  // Every possible bus (top→bottom). SDA/SCL are I²C bus 0, SDA2/SCL2 the optional
  // second bus; V+, GND, 3V3 are shared; /OE is shared unless split per bus (/OE2).
  var BUSES = [
    { key: 'vplus', label: 'V+ 5–6V', cls: 'vplus' },
    { key: 'gnd',   label: 'GND',     cls: 'gnd' },
    { key: 'v3',    label: '3V3',     cls: 'v3' },
    { key: 'sda',   label: 'SDA',     cls: 'sda' },
    { key: 'scl',   label: 'SCL',     cls: 'scl' },
    { key: 'sda2',  label: 'SDA2',    cls: 'sda2' },
    { key: 'scl2',  label: 'SCL2',    cls: 'scl2' },
    { key: 'oe',    label: '/OE',     cls: 'oe' },
    { key: 'oe2',   label: '/OE2',    cls: 'oe2' }
  ];
  // The shared /OE line is drawn/exposed when a board relies on it: any bus-0 board,
  // or a bus-1 board with no split /OE2.
  function needSharedOe(m) { return m.useBus0 || (m.useBus1 && !m.splitOe); }

  // The ESP pins that drop onto buses (GND-only for a direct-only rig). Only the
  // buses actually carrying a board appear, so an empty bus adds no pins/rails.
  function espPins(m) {
    if (!m.usePca) return [{ key: 'gnd', label: 'GND' }];
    var a = [{ key: 'v3', label: '3V3' }, { key: 'gnd', label: 'GND' }];
    if (m.useBus0) a.push({ key: 'sda', label: 'SDA', gpio: m.sda }, { key: 'scl', label: 'SCL', gpio: m.scl });
    if (m.useBus1) a.push({ key: 'sda2', label: 'SDA2', gpio: m.sda2 }, { key: 'scl2', label: 'SCL2', gpio: m.scl2 });
    if (needSharedOe(m)) a.push({ key: 'oe', label: '/OE', gpio: m.oe });
    if (m.splitOe) a.push({ key: 'oe2', label: '/OE2', gpio: m.oe2 });
    return a;
  }
  function espWidth(m) { return Math.max(172, 2 * TAP_PAD + (espPins(m).length - 1) * PITCH); }

  // The rails actually drawn for a given model (only buses that carry a board).
  function activeRails(m) {
    return BUSES.filter(function (b) {
      switch (b.key) {
        case 'v3': return m.usePca;
        case 'sda': case 'scl': return m.useBus0;
        case 'sda2': case 'scl2': return m.useBus1;
        case 'oe': return needSharedOe(m);
        case 'oe2': return m.splitOe;
        default: return true;  // vplus, gnd
      }
    });
  }
  function computeRailY(m) {
    var map = {};
    activeRails(m).forEach(function (b, i) { map[b.key] = BUS_TOP + i * BUS_STEP; });
    return map;
  }
  function railY(m, key) { return m.railY && m.railY[key] != null ? m.railY[key] : BUS_TOP; }
  function boardTopFor(m) { return BUS_TOP + activeRails(m).length * BUS_STEP + BAND_GAP; }

  // A board taps SDA/SCL of its own I²C bus, and /OE of its bus when /OE is split.
  function sdaKey(bus) { return bus === 1 ? 'sda2' : 'sda'; }
  function sclKey(bus) { return bus === 1 ? 'scl2' : 'scl'; }
  function oeKey(m, bus) { return (m.splitOe && bus === 1) ? 'oe2' : 'oe'; }

  // The compact role shown on a channel pad. A finger carries no fret number on
  // this build — the carriage chooses the fret, so one finger serves them all.
  function roleWord(sv) {
    switch (sv.function) {
      case 'finger': return 'Finger';
      case 'pluck': return 'Pluck';
      case 'strum': return 'Strum';
      case 'strumLift': return 'Lift';
      case 'damper': return 'Damp';
      case 'aux': return 'Aux';
      default: return String(sv.function);
    }
  }

  // The GPIO carried by a signal rail, whether it is required-but-missing, and the
  // rail's right-hand label (signal buses append their GPIO, e.g. "SDA2·38").
  function railGpio(m, key) { return ({ sda: m.sda, scl: m.scl, sda2: m.sda2, scl2: m.scl2, oe: m.oe, oe2: m.oe2 })[key]; }
  function railMissing(m, key) {
    if (key === 'sda' || key === 'scl') return m.useBus0 && railGpio(m, key) < 0;
    if (key === 'sda2' || key === 'scl2') return m.useBus1 && railGpio(m, key) < 0;
    if (key === 'oe2') return m.splitOe && m.oe2 < 0;
    return false;
  }
  function railLabel(m, b) {
    var g = railGpio(m, b.key);
    return g !== undefined ? b.label + '·' + (g >= 0 ? g : '—') : b.label;
  }

  function junction(cls, x, y) { return svg('circle', { class: 'wire-dot ' + cls, cx: x, cy: y, r: 2.6 }); }

  // ---- diagram construction -------------------------------------------------
  function buildDiagram(p, m) {
    m.railY = computeRailY(m);        // rail Y positions for THIS model (skips unused buses)
    m.boardTop = boardTopFor(m);      // boards start below the whole bus band

    // Row items left→right: a Direct-GPIO group (if any) then one box per chip.
    var items = [];
    if (m.direct.length) items.push({ type: 'direct' });
    m.boards.forEach(function (b) { items.push({ type: 'board', b: b }); });

    // A right gutter holds the bus labels (SDA2·38 …) so they never clip.
    var LABEL_GUTTER = 82;
    // Compact: boards start at the left, directly under the ESP. Their taps
    // interleave with the ESP's (half-pitch offset) rather than aligning, so no
    // horizontal space is wasted shifting the boards aside.
    var topZoneRight = ESP_X + espWidth(m) + 16 + PSU_W;
    var boardX0 = ESP_X;
    var boardsRight = items.length ? boardX0 + items.length * (BOARD_W + BOARD_GAP) - BOARD_GAP : boardX0;
    var contentRight = Math.max(boardsRight, topZoneRight);
    var VB_W = contentRight + LABEL_GUTTER;
    var VB_H = m.boardTop + BOARD_H + 30;
    var busRight = contentRight + 4;

    var root = svg('svg', {
      class: 'wire-svg', viewBox: '0 0 ' + VB_W + ' ' + VB_H,
      preserveAspectRatio: 'xMidYMid meet', role: 'group',
      // Render at natural width and scroll (like the fretboard) so a many-board
      // harness stays legible instead of shrinking to fit the container.
      style: 'min-width:' + VB_W + 'px',
      'aria-label': 'Wiring map of the ESP32 and PCA9685 boards'
    });

    // Horizontal buses spanning the whole width (drawn first, under everything).
    activeRails(m).forEach(function (b) {
      var y = railY(m, b.key);
      root.appendChild(svg('line', { class: 'wire-rail ' + b.cls, x1: 18, y1: y, x2: busRight, y2: y }));
      root.appendChild(svg('text', { class: 'wire-raillabel' + (railMissing(m, b.key) ? ' missing' : ''),
        x: busRight + 8, y: y + 3, 'text-anchor': 'start', text: railLabel(m, b) }));
    });

    root.appendChild(buildEsp(p, m));
    root.appendChild(buildPsu(p, m));

    items.forEach(function (it, i) {
      var x = boardX0 + i * (BOARD_W + BOARD_GAP);
      if (it.type === 'direct') root.appendChild(buildDirectBox(p, m, x));
      else root.appendChild(buildBoard(p, m, it.b, x));
    });

    return root;
  }

  // ESP32-S3 module (top-left), dropping its signal pins onto the buses. With a
  // second I²C bus it also exposes SDA2 / SCL2 (the ESP32-S3's second controller).
  function buildEsp(p, m) {
    var g = svg('g', { class: 'wire-mod' });
    var x = ESP_X, y = TOP_Y, w = espWidth(m), hh = 82;
    g.appendChild(svg('rect', { class: 'wire-esp', x: x, y: y, width: w, height: hh, rx: 9 }));
    g.appendChild(svg('text', { class: 'wire-title light', x: x + 12, y: y + 22, text: 'ESP32-S3' }));
    g.appendChild(svg('text', { class: 'wire-sub light', x: x + 12, y: y + 38, text: 'DevKitC-1 · 3.3 V logic' }));

    // Colour-coded taps drop onto the buses (GND tie always; 3V3 / I²C / OE with a
    // PCA; SDA2/SCL2 with a second bus; /OE2 when /OE is split). The pin names live
    // on the right-hand rail labels, so these closely-spaced taps stay uncluttered.
    espPins(m).forEach(function (pin, i) {
      var px = espTapX(i);
      var by = railY(m, pin.key);
      var missing = pin.gpio !== undefined && pin.gpio < 0;
      g.appendChild(svg('line', { class: 'wire-lead ' + pin.key + (missing ? ' missing' : ''), x1: px, y1: y + hh, x2: px, y2: by }));
      g.appendChild(junction(pin.key, px, by));
    });
    return g;
  }

  // Separate 5–6 V servo supply feeding the V+ and GND buses.
  function buildPsu(p, m) {
    var g = svg('g', { class: 'wire-mod' });
    var x = ESP_X + espWidth(m) + 16, y = TOP_Y, w = PSU_W, hh = 60;
    g.appendChild(svg('rect', { class: 'wire-psu', x: x, y: y, width: w, height: hh, rx: 9 }));
    g.appendChild(svg('text', { class: 'wire-title light', x: x + 12, y: y + 22, text: 'Servo PSU' }));
    g.appendChild(svg('text', { class: 'wire-sub light', x: x + 12, y: y + 38, text: '5–6 V · separate' }));
    [ { key: 'vplus', label: 'V+' }, { key: 'gnd', label: 'GND' } ].forEach(function (pin, i) {
      var px = snapTapX(x + w * (i ? 0.7 : 0.3));   // on the ESP grid → clears board taps
      var by = railY(m, pin.key);
      g.appendChild(svg('line', { class: 'wire-lead ' + pin.key, x1: px, y1: y + hh, x2: px, y2: by }));
      g.appendChild(junction(pin.key, px, by));
      g.appendChild(svg('text', { class: 'wire-pinlabel light', x: px, y: y + hh - 5, 'text-anchor': 'middle', text: pin.label }));
    });
    return g;
  }

  // One PCA9685 breakout: taps the buses at its top edge (SDA/SCL — and /OE when
  // split — on its own bus), lays out 16 channels, and names the string(s) it serves.
  function buildBoard(p, m, bd, x) {
    var g = svg('g', { class: 'wire-mod' });
    var key = bd.key, top = m.boardTop, used = 0;
    m.byBoard[key].forEach(function (l) { if (l.length) used++; });

    // Top-edge input pins on the half-pitch grid (so they interleave with the ESP
    // taps); SDA/SCL — and /OE when split — tap this board's own bus.
    var pinKeys = ['vplus', 'gnd', 'v3', sdaKey(bd.bus), sclKey(bd.bus), oeKey(m, bd.bus)];
    pinKeys.forEach(function (pk, i) {
      var px = boardTapX(x, i);
      var by = railY(m, pk);
      g.appendChild(svg('line', { class: 'wire-lead ' + pk, x1: px, y1: top, x2: px, y2: by }));
      g.appendChild(junction(pk, px, by));
    });

    g.appendChild(svg('rect', { class: 'wire-board', x: x, y: top, width: BOARD_W, height: BOARD_H, rx: 9 }));
    g.appendChild(svg('text', { class: 'wire-title', x: x + 12, y: top + 22, text: 'PCA9685 #' + bd.board }));
    g.appendChild(svg('text', { class: 'wire-sub', x: x + BOARD_W - 12, y: top + 22, 'text-anchor': 'end', text: hex2(0x40 + bd.board) }));
    // Which bus + string(s) this board serves (a board may be shared across strings).
    g.appendChild(svg('text', { class: 'wire-sub host', x: x + 12, y: top + 39,
      text: (m.useBus1 ? 'Bus ' + bd.bus + ' · ' : '') + hostedStrings(m, key) }));
    g.appendChild(svg('text', { class: 'wire-sub', x: x + BOARD_W - 12, y: top + 39, 'text-anchor': 'end', text: used + '/16' }));

    // 16 channels in two columns of eight: pin/channel at the left, string·role at
    // the right (so "0 → S1·Lift" reads at a glance, with no overlapping text).
    var padW = (BOARD_W - 24 - 8) / 2, padH = 20, gapx = 8, gapy = 5;
    var gx0 = x + 12, gy0 = top + 50;
    for (var ch = 0; ch < 16; ch++) {
      var col = ch < 8 ? 0 : 1, row = ch % 8;
      var px = gx0 + col * (padW + gapx), py = gy0 + row * (padH + gapy);
      g.appendChild(buildChannel(m, key, ch, px, py, padW, padH));
    }
    return g;
  }

  function buildChannel(m, key, ch, px, py, pw, ph) {
    var list = m.byBoard[key][ch];
    var sv = list[0];
    var cls = 'wire-pad';
    if (!sv) cls += ' free';
    else {
      cls += ' used ' + (FN[sv.function] ? FN[sv.function].cls : 'fn-aux');
      if (sv.enabled === false) cls += ' off';
      if (list.length > 1) cls += ' dup';
    }
    var cell = svg('g', { class: 'wire-cell' });
    cell.appendChild(svg('rect', { class: cls, x: px, y: py, width: pw, height: ph, rx: 5 }));
    var midY = py + ph / 2 + 3.5;
    // Left = the PCA pin/channel number; right = string·role (e.g. S1·Lift).
    cell.appendChild(svg('text', { class: 'wire-chpin' + (sv ? ' on' : ''), x: px + 7, y: midY, text: ch }));
    if (sv) cell.appendChild(svg('text', { class: 'wire-chlabel', x: px + pw - 7, y: midY,
      'text-anchor': 'end', text: stringTag(sv) + '·' + roleWord(sv) }));
    // Native SVG tooltip: full description on hover.
    var tip = 'Channel ' + ch + ' — ' + (sv
      ? servoLabel(sv, m.auxOrder[m.servos.indexOf(sv)] || 0) + (sv.enabled === false ? ' (disabled)' : '') +
        (list.length > 1 ? ' — CONFLICT: ' + list.length + ' servos here' : '')
      : 'free');
    cell.appendChild(svg('title', null, tip));
    return cell;
  }

  // Direct-GPIO servos grouped in a board-sized box (signal from ESP GPIO, power
  // from the shared V+/GND buses).
  function buildDirectBox(p, m, x) {
    var g = svg('g', { class: 'wire-mod' });
    var top = m.boardTop;
    // Tap only V+ and GND (on the half-pitch grid like a board).
    ['vplus', 'gnd'].forEach(function (key, i) {
      var px = boardTapX(x, i);
      var by = railY(m, key);
      g.appendChild(svg('line', { class: 'wire-lead ' + key, x1: px, y1: top, x2: px, y2: by }));
      g.appendChild(junction(key, px, by));
    });
    g.appendChild(svg('rect', { class: 'wire-board direct', x: x, y: top, width: BOARD_W, height: BOARD_H, rx: 9 }));
    g.appendChild(svg('text', { class: 'wire-title', x: x + 12, y: top + 22, text: 'Direct GPIO' }));
    g.appendChild(svg('text', { class: 'wire-sub', x: x + BOARD_W - 12, y: top + 22, 'text-anchor': 'end', text: '×' + m.direct.length }));

    var y = top + 46, lh = 24, shown = 0, max = 9;
    m.direct.forEach(function (s) {
      if (shown >= max) return;
      shown++;
      var dup = s.gpio >= 0 && (m.byGpio[s.gpio] || []).length > 1;
      var fn = FN[s.function] || FN.aux;
      var lbl = servoLabel(s, m.auxOrder[m.servos.indexOf(s)] || 0);
      g.appendChild(svg('rect', { class: 'wire-swatch ' + fn.cls, x: x + 12, y: y - 10, width: 12, height: 12, rx: 3 }));
      g.appendChild(svg('text', { class: 'wire-directpin' + (dup ? ' dup' : ''), x: x + 30, y: y, text: (s.gpio >= 0 ? 'GPIO' + s.gpio : 'GPIO —') }));
      g.appendChild(svg('text', { class: 'wire-directlbl', x: x + BOARD_W - 12, y: y, 'text-anchor': 'end', text: lbl.replace(/^String /, 'S').replace(' · ', ' ') }));
      y += lh;
    });
    if (m.direct.length > max)
      g.appendChild(svg('text', { class: 'wire-sub', x: x + 12, y: y + 2, text: '+ ' + (m.direct.length - max) + ' more…' }));
    return g;
  }

  // ---- HTML surrounds (legend, summary, controls) ---------------------------
  function legendCard() {
    function sw(cls, label) { return h('span.wire-lg', [h('span.wire-lg-sw.' + cls), h('span', label)]); }
    function rail(cls, label) { return h('span.wire-lg', [h('span.wire-lg-rail.' + cls), h('span', label)]); }
    return h('div.card', [
      h('h2', 'Legend'),
      h('div.wire-legend', [
        sw('fn-finger', 'Finger'), sw('fn-pluck', 'Plucker'), sw('fn-strum', 'Strum'),
        sw('fn-lift', 'Strum lift'), sw('fn-damper', 'Damper'), sw('fn-aux', 'Auxiliary'),
        h('span.wire-lg', [h('span.wire-lg-sw.free'), h('span', 'Free channel')])
      ]),
      h('div.wire-legend', [
        rail('vplus', 'V+ 5–6 V servo rail'), rail('gnd', 'GND (common)'), rail('v3', '3V3 logic'),
        rail('sda', 'I²C bus 0 SDA'), rail('scl', 'I²C bus 0 SCL'),
        rail('sda2', 'I²C bus 1 SDA (SDA2)'), rail('scl2', 'I²C bus 1 SCL (SCL2)'),
        rail('oe', '/OE safety (bus 0)'), rail('oe2', '/OE bus 1 (OE2)')
      ]),
      h('p.muted', 'Each labelled channel is where one servo plugs in (signal + V+ + GND). On a pad the left is the ' +
        'PCA pin/channel and the right is its string·role: a fret (S1·f3), or Pluck / Strum / Lift (strum-lift) / ' +
        'Damp (damper) / Aux (GLB = global auxiliary). A single PCA9685 can host several strings — each pin carries ' +
        'its own string, and the board header lists the strings it serves. Hover a channel for the full description.')
    ]);
  }

  function summaryCard(p, m, issues) {
    var onPca = m.pca.length, direct = m.direct.length;
    var stat = function (label, value) { return h('div.stat', [h('div.stat-label', label), h('div.stat-value', value)]); };
    var gp = function (g) { return g >= 0 ? 'GPIO' + g : '—'; };
    var busAddrs = function (bus) {
      return m.boards.filter(function (b) { return b.bus === bus; })
        .map(function (b) { return hex2(0x40 + b.board); }).join(', ') || '—';
    };
    var nBus = function (bus) { return m.boards.filter(function (b) { return b.bus === bus; }).length; };
    var cells = [
      stat('Instrument', (p.instrument && p.instrument.name) || '—'),
      stat('Strings', String((p.instrument && p.instrument.stringCount) || (p.strings || []).length)),
      stat('I²C buses', m.useBus1 ? '2' : '1'),
      stat('PCA9685 boards', m.useBus1 ? (m.boards.length + ' (b0:' + nBus(0) + ' · b1:' + nBus(1) + ')') : String(m.boards.length)),
      stat('Servos on PCA', String(onPca)),
      stat('Direct-GPIO servos', String(direct))
    ];
    if (m.useBus1) {
      cells.push(stat('Bus 0 · addr', busAddrs(0)));
      cells.push(stat('Bus 0 · SDA/SCL', gp(m.sda) + ' / ' + gp(m.scl)));
      cells.push(stat('Bus 1 · addr', busAddrs(1)));
      cells.push(stat('Bus 1 · SDA/SCL', gp(m.sda2) + ' / ' + gp(m.scl2)));
    } else {
      cells.push(stat('I²C addresses', busAddrs(0)));
      cells.push(stat('SDA / SCL', gp(m.sda) + ' / ' + gp(m.scl)));
    }
    if (m.splitOe) { cells.push(stat('/OE bus 0', gp(m.oe))); cells.push(stat('/OE bus 1 (OE2)', gp(m.oe2))); }
    else cells.push(stat('/OE safety', gp(m.oe)));
    var estop = (p.pins || []).filter(function (x) { return x.signal === 'ESTOP'; })[0];
    cells.push(stat('Hardware E-stop', estop ? gp(estop.gpio) +
      (p.board && p.board.estopNormallyClosed ? ' · NC' : ' · NO') : 'not declared'));
    var grid = h('div.grid', cells);
    var kids = [h('div.card-head', [h('h2', 'Harness summary'),
      h('span.muted', 'derived live from the current configuration')]), grid];
    if (issues.length) {
      kids.push(h('h3', 'Wiring checks'));
      issues.forEach(function (is) {
        kids.push(h('div.err-item' + (is.sev === 'warning' ? '.warn' : ''), [
          h('div.err-head', [h('span.pill.mini.' + (is.sev === 'warning' ? 'warn' : 'error'), is.sev)]),
          h('div.err-reason', is.msg)
        ]));
      });
    } else {
      kids.push(h('div.pill.ok', 'No wiring conflicts detected.'));
    }
    return h('div.card', kids);
  }

  // Bulk-capacitor range for a board, sized to how many micro-servos can start at
  // once on it (each servo's in-rush hits this board's own capacitor):
  //   ~4 → 1000–2200 µF · ~8 → 2200–4700 µF · ~16 → 4700–10000 µF.
  // EMPIRICAL STARTING VALUES for small micro-servos (SG90 class) — not an
  // electrical law. Two powerful digital servos can out-draw eight micro-servos:
  // size from the datasheet stall/peak current with C ≈ I·Δt/ΔV, then confirm at
  // the bench (hardware/POWER_AND_SAFETY.md §capacitors).
  function capRange(n) {
    if (n <= 4) return '1000–2200 µF';
    if (n <= 8) return '2200–4700 µF';
    return '4700–10000 µF';  // up to a PCA9685's 16 channels
  }

  // Power-distribution advice + a per-board bulk-capacitor recommendation, sized to
  // how many micro-servos sit on each board — its own worst-case simultaneous start.
  // A per-board start cap (Timing step) hard-limits that and can lower the cap; the
  // whole-instrument global cap is NOT used, since the capacitor must handle the
  // board's own load whatever the software throttle does.
  function powerAdviceCard(p, m) {
    var pw = p.power || {};
    var boardCap = pw.maxConcurrentPerBoard | 0;  // 0 = no per-board limit
    var anyAboveMin = false;
    var rows = m.boards.map(function (b) {
      var chans = m.byBoard[b.key] || [];
      var count = 0;
      for (var c = 0; c < 16; c++) count += (chans[c] ? chans[c].length : 0);
      var worst = boardCap > 0 ? Math.min(count, boardCap) : count;
      if (worst > 4) anyAboveMin = true;
      return h('tr', [
        h('td', 'PCA ' + b.board + ' · ' + hex2(0x40 + b.board) + (m.useBus1 ? ' · bus ' + b.bus : '')),
        h('td', String(count)),
        h('td', boardCap > 0 ? ('≤ ' + worst) : String(worst)),
        h('td.cap-uf', capRange(worst))
      ]);
    });
    var kids = [
      h('div.card-head', [h('h2', 'Power wiring'), h('span.muted', 'protect the 5–6 V servo rail')]),
      h('ul.advice-list', [
        h('li', ['Run a ', h('strong', 'direct line from the power supply to each PCA9685'),
          '’s V+/GND input (star wiring) — don’t daisy-chain the power from one board to the ' +
          'next, so one board’s in-rush can’t sag its neighbours. Fuse the rail at the PSU and, ' +
          'ideally, ', h('strong', 'each branch'), ' at the distribution point.']),
        h('li', ['Add a ', h('strong', 'bulk capacitor across each PCA9685’s V+/GND'),
          ', sized to how many micro-servos can start at once on that board: ',
          h('strong', '~4 → 1000–2200 µF'), ', ', h('strong', '~8 → 2200–4700 µF'), ', ',
          h('strong', '~16 → 4700–10000 µF'), '. These are ', h('strong', 'empirical starting values ' +
          'for small micro-servos'), ' — powerful digital servos draw far more: size from the ' +
          'datasheet peak current (C ≈ I·Δt/ΔV) and confirm at the bench. The firmware’s start ' +
          'governor spreads the peaks but is no substitute for a properly sized supply.']),
        h('li', ['Pair it with a ', h('strong', '100 nF ceramic'),
          ' across the same V+/GND to filter high-frequency noise — this limits ESP32 ' +
          'resets/crashes, especially when the logic shares the servo supply.']),
        h('li', 'Keep the servo supply separate from the ESP32 3.3 V logic — share only GND.'),
        h('li', ['Make ', h('strong', '/OE fail-safe'), ': a pull-up to 3.3 V on the /OE bus so the ' +
          'outputs stay disabled with the ESP32 absent, resetting or unplugged, driven low through ' +
          'an open-drain stage rather than pushed by the GPIO alone. A hardware E-stop chain should ' +
          'also drop the servo rail (contactor) — /OE cannot stop direct-GPIO servos. Reference ' +
          'circuit: hardware/POWER_AND_SAFETY.md.'])
      ])
    ];
    if (m.boards.length) {
      kids.push(h('table.cap-table', [
        h('thead', h('tr', [h('th', 'Board'), h('th', 'Servos'),
          h('th', 'Can start at once'), h('th', 'Bulk cap')])),
        h('tbody', rows)
      ]));
      if (anyAboveMin && boardCap === 0)
        kids.push(h('p.muted', 'Tip: a per-board start cap on the Timing step limits how many ' +
          'servos start at once, so a smaller capacitor per board is enough.'));
    }
    return h('div.card', kids);
  }

  // ---- stepper drivers ------------------------------------------------------
  // The carriage side of the harness. It is deliberately a TABLE rather than part
  // of the PCA SVG: a STEP/DIR driver is wired point-to-point (four signals + the
  // shared ENABLE + its own motor rail), so what the builder needs at the bench is
  // "which ESP32 pin goes to which driver terminal", not another bus picture.
  function driversCard(p, m) {
    if (!m.axes.length) return null;
    function pinCell(gpio, optional) {
      if (gpio >= 0) return h('td', h('code', 'GPIO' + gpio));
      return h('td', h('span.pill' + (optional ? '' : '.warn'), optional ? 'none' : 'unassigned'));
    }
    var rows = m.axes.map(function (a) {
      if (!a.enabled) {
        return h('tr.muted', [h('td', 'Axis ' + (a.index + 1)),
          h('td', { colspan: 4 }, 'disabled in the profile — no driver, no pins')]);
      }
      return h('tr', [
        h('td', h('strong', 'Axis ' + (a.index + 1))),
        pinCell(a.step), pinCell(a.dir), pinCell(a.home), pinCell(a.limit, true)
      ]);
    });
    return h('div.card', [
      h('div.card-head', [h('h2', 'Stepper drivers'),
        h('span.muted', m.activeAxes.length + ' axis/axes · one STEP/DIR driver each')]),
      h('p.muted', 'One driver per string: the carriage it moves is what chooses the fret. ' +
        'STEP and DIR come from the ESP32; HOME is the reference endstop every axis must find ' +
        'before it may play; LIMIT is the opposite endstop that catches a missed HOME. All the ' +
        'drivers share one ENABLE line, so de-energising is all-or-nothing.'),
      h('table.cap-table', [
        h('thead', h('tr', [h('th', 'Axis'), h('th', 'STEP'), h('th', 'DIR'),
          h('th', 'HOME'), h('th', 'LIMIT')])),
        h('tbody', rows)
      ]),
      h('ul.advice-list', [
        h('li', ['Shared ', h('strong', 'ENABLE'), ': ',
          m.enable >= 0 ? h('code', 'GPIO' + m.enable) : h('span.pill.warn', 'unassigned'),
          ' — active-low on the common drivers (A4988 / DRV8825 / TMC2209), so it is ',
          h('strong', 'HIGH = drivers off'), ' at boot and after a hard stop.']),
        h('li', ['Hardware ', h('strong', 'E-stop'), ': ',
          m.estop >= 0 ? h('code', 'GPIO' + m.estop) : h('span.pill.warn', 'not declared'),
          m.estop >= 0 ? (m.estopNc
            ? ' wired as a NORMALLY-CLOSED loop — a press, a cut wire or an unplugged connector all read STOP (fail-safe).'
            : ' wired as a normally-OPEN button — a cut wire reads "running". Prefer the NC loop.')
            : ' — a software stop is not a safety function.']),
        h('li', ['The E-stop chain should ', h('strong', 'cut the motor rail AND force ENABLE inactive'),
          ': a stepper holds its position by burning current in the coils, so a driver left ' +
          'energised is still hot (and still holding) after the firmware has stopped. ' +
          'Reference circuit: hardware/POWER_AND_SAFETY.md.']),
        h('li', ['Give the motors their ', h('strong', 'own supply'),
          ' (typically 12–24 V), separate from the 5–6 V servo rail and from the ESP32 logic — ' +
          'share only GND. Set each driver’s current limit (Vref) to the motor’s rated phase ' +
          'current BEFORE the first motion, or the first homing seek is the expensive kind.']),
        h('li', ['Put a ', h('strong', 'bulk capacitor (≥100 µF, rated well above the rail)'),
          ' across each driver’s motor supply, close to the driver: an axis accelerating draws its ' +
          'peak in the first milliseconds, and the whole chord accelerates together. The firmware’s ' +
          'start governor spreads those starts, but it cannot invent supply headroom.'])
      ])
    ]);
  }

  // Serialize the live SVG and offer it as a download (a portable bench reference).
  function downloadSvg(host) {
    var node = host.querySelector('.wire-svg');
    if (!node) return;
    var clone = node.cloneNode(true);
    clone.setAttribute('xmlns', SVGNS);
    // Inline the computed theme colours so the file renders outside the app.
    inlineStyles(node, clone);
    var data = '<?xml version="1.0" encoding="UTF-8"?>\n' + new XMLSerializer().serializeToString(clone);
    var blob = new Blob([data], { type: 'image/svg+xml' });
    var url = URL.createObjectURL(blob);
    var name = ((GMB.state.profile.instrument && GMB.state.profile.instrument.name) || 'instrument');
    var a = document.createElement('a');
    a.href = url; a.download = GMB.slug(name) + '-wiring.svg';
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(function () { URL.revokeObjectURL(url); }, 1000);
    GMB.toast('Wiring diagram downloaded.', 'ok');
  }
  // Copy computed presentation properties onto the clone so the standalone SVG
  // keeps its colours (the external stylesheet is not embedded).
  function inlineStyles(src, dst) {
    var props = ['fill', 'stroke', 'stroke-width', 'stroke-dasharray', 'opacity',
      'font-size', 'font-weight', 'font-family', 'text-anchor'];
    var cs = getComputedStyle(src);
    var decl = props.map(function (pr) { return pr + ':' + cs.getPropertyValue(pr); }).join(';');
    if (src.nodeType === 1) dst.setAttribute('style', decl);
    var sc = src.children || [], dc = dst.children || [];
    for (var i = 0; i < sc.length; i++) if (dc[i]) inlineStyles(sc[i], dc[i]);
  }

  // ---- view -----------------------------------------------------------------
  function render(host) {
    var p = GMB.state.profile;
    var servos = p.servos || [];

    host.appendChild(h('div.card', [
      h('div.card-head', [h('h2', 'Wiring'),
        h('span.muted', 'graphical harness — ESP32-S3 + PCA9685(s), adapts to the configuration')]),
      h('p.muted', 'A schematic of how the current instrument is wired: the ESP32-S3, one STEP/DIR ' +
        'driver per carriage axis with its HOME/LIMIT endstops and the shared ENABLE, a separate ' +
        '5–6 V servo supply, one PCA9685 per board actually used (at its I²C address), the shared ' +
        'power + /OE buses, and any direct-GPIO servos. Boards can be split across the ESP32-S3’s two ' +
        'I²C buses (SDA/SCL and SDA2/SCL2). Each occupied channel is labelled with its string and role, so a ' +
        'PCA9685 shared across several strings stays unambiguous. It updates with each change made on the ' +
        'Setup page and the GPIO sub-tab.')
    ]));

    if (!servos.length) {
      host.appendChild(h('div.card', [h('div.pill.warn', 'No servos configured yet.'),
        h('p.muted', 'Set the instrument up first (Setup page) — the wiring map is built from its axes and servos.'),
        h('div.row', [GMB.button('Go to setup', function () { GMB.navigate('wizard'); }, 'primary')])]));
      return;
    }

    var m = buildModel(p);
    var issues = findIssues(p, m);

    if (!m.usePca && !m.direct.length) {
      host.appendChild(h('div.note-box', 'The servos are configured but none has a signal source yet.'));
    }

    // The diagram (scrolls horizontally on narrow screens, like the fretboard).
    host.appendChild(h('div.card.wire-card', [
      h('div.wire-toolbar', [
        h('span.muted', m.activeAxes.length + ' stepper axis/axes · ' + m.boards.length +
          ' PCA board(s) on ' + (m.useBus1 ? '2 I²C buses' : '1 I²C bus') +
          ' · ' + m.pca.length + ' PCA servo(s) · ' + m.direct.length + ' direct'),
        h('span.spacer'),
        GMB.button('Download SVG', function () { downloadSvg(host); }, 'ghost')
      ]),
      h('div.wire-scroll', buildDiagram(p, m))
    ]));

    var drivers = driversCard(p, m);
    if (drivers) host.appendChild(drivers);
    host.appendChild(summaryCard(p, m, issues));
    host.appendChild(powerAdviceCard(p, m));
    host.appendChild(legendCard());
  }

  GMB.views.wiring = { render: render };
  // The profile → wiring model is shared with the Power & safety and I²C & PCA
  // sub-tabs (boards per bus, channel occupancy, board-level signal GPIOs).
  GMB.wiringModel = buildModel;
})(window);
