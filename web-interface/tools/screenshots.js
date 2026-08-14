/*
 * screenshots.js — regenerate img/screenshots/*.png from the real interface.
 *
 * The UI falls back to its in-memory mock backend when no device answers (see
 * js/api.js), so opening index.html from file:// gives a complete, deterministic
 * demo instrument — a 4-string GCEA ukulele with four carriages — and every
 * screenshot in the documentation is taken from it. No device, no build step.
 *
 * Usage (Playwright is NOT a project dependency — install it wherever you like):
 *
 *   npm i playwright && npx playwright install chromium
 *   node web-interface/tools/screenshots.js
 *
 * Set CHROMIUM_PATH to use an already-installed Chromium instead, and
 * PLAYWRIGHT_MODULE to point at a Playwright installed outside this repo:
 *
 *   PLAYWRIGHT_MODULE=/tmp/pw/node_modules/playwright \
 *   CHROMIUM_PATH=/opt/pw-browsers/chromium/chrome-linux/chrome \
 *   node web-interface/tools/screenshots.js
 *
 * Each page is shot full-height with the viewport fitted to the rendered content,
 * so the images carry no band of empty background. Keep the file names stable:
 * they are referenced from README.md and docs/WEB_INTERFACE.md.
 */
'use strict';

const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const path = require('path');
const fs = require('fs');

const ROOT = path.resolve(__dirname, '..', '..');
const OUT = path.join(ROOT, 'img', 'screenshots');
const URL = 'file://' + path.join(ROOT, 'web-interface', 'index.html');
const WIDTH = 1400;                    // desktop layout, single column of cards
const SCALE = 1.5;                     // crisp text without 4 MB PNGs

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

(async () => {
  fs.mkdirSync(OUT, { recursive: true });
  const browser = await chromium.launch(
    process.env.CHROMIUM_PATH ? { executablePath: process.env.CHROMIUM_PATH } : {});
  const page = await browser.newPage({
    viewport: { width: WIDTH, height: 900 },
    deviceScaleFactor: SCALE,
    colorScheme: 'light',
  });
  const errors = [];
  page.on('pageerror', (e) => errors.push(String(e)));

  await page.goto(URL);
  await page.waitForSelector('.nav-item');
  await sleep(700);

  // `fit` grows the viewport to the view's real height before a full-page shot.
  // The modal shots pass fit:false: the overlay is viewport-sized by design.
  async function shot(name, opts) {
    const fit = !opts || opts.fit !== false;
    await sleep(350);
    if (fit) {
      const height = await page.evaluate(() => {
        const view = document.getElementById('view');
        const box = view && view.getBoundingClientRect();
        return Math.ceil(Math.max(box ? box.bottom + window.scrollY + 20 : 0, 560));
      });
      await page.setViewportSize({ width: WIDTH, height: Math.min(height, 4200) });
      await sleep(250);
    }
    await page.screenshot({ path: path.join(OUT, name + '.png'), fullPage: fit });
    await page.setViewportSize({ width: WIDTH, height: 900 });
    console.log('  ' + name + '.png');
  }

  const view = async (id) => { await page.evaluate((v) => GMB.navigate(v), id); await sleep(600); };
  // The setup wizard is a numbered stepper; goto() accepts an index or a label.
  // It only exists once the Setup view is mounted, so navigate first — stepping a
  // view that is not on screen silently shoots whatever page IS.
  const step = async (label) => {
    await page.evaluate((s) => {
      if (GMB.state.current !== 'wizard') GMB.navigate('wizard');
      return GMB.views.wizard.goto(s);
    }, label);
    await sleep(800);
  };
  const sub = async (label) => { await page.click('.subtab:text-is("' + label + '")'); await sleep(700); };

  console.log('Instrument');
  await view('fretboard');
  await sleep(900);                    // let a status frame land so carriages draw
  await shot('instrument');

  console.log('Setup');
  await view('wizard');
  await sleep(900);                    // the wizard fetches the board profile first
  for (const [label, name] of [
    ['Identification', 'setup-identification'],
    ['Board', 'setup-board'],
    ['Pins', 'setup-pins'],
    ['Mechanics', 'setup-mechanics'],
    ['Homing', 'setup-homing'],
    ['Servos', 'setup-servos'],
    ['Notes', 'setup-notes'],
    ['Test', 'setup-test'],
    ['Validation', 'setup-validation'],
  ]) {
    await step(label);
    await shot(name);
  }

  console.log('Wiring & GPIO');
  await view('hardware');
  await sub('Harness');
  await shot('wiring');
  await sub('Power & safety');
  await shot('wiring-power');
  await sub('I²C & PCA');
  await shot('wiring-i2c');
  await sub('GPIO pins');
  await shot('pins');
  await sub('Commissioning');
  await shot('commissioning');

  console.log('Settings modal');
  await view('fretboard');                       // calm backdrop behind the overlay
  await page.evaluate(() => GMB.openSettings('network'));
  await sleep(900);
  await shot('settings-network', { fit: false });
  await page.click('.settings-tab:text-is("Diagnostics")');
  await sleep(1400);                             // first /api/diagnostics poll
  await shot('settings-diagnostics', { fit: false });
  await page.click('.settings-tab:text-is("Advanced")');
  await sleep(1400);
  await shot('settings-advanced', { fit: false });

  // The Advanced tab scrolls; bring each card into view for its own capture.
  async function scrollTo(heading) {
    await page.evaluate((t) => {
      const body = document.getElementById('settings-body');
      if (!body) return;
      const target = Array.from(body.querySelectorAll('h2,h3'))
        .find((el) => el.textContent.includes(t));
      if (target) body.scrollTop = target.offsetTop - body.offsetTop - 12;
    }, heading);
    await sleep(500);
  }
  await scrollTo('GMB identity');
  await shot('sysex', { fit: false });
  await scrollTo('MIDI monitor');
  await shot('midi-monitor', { fit: false });

  await browser.close();

  if (errors.length) {
    console.log('\nPage errors (the CORS/fetch failures of file:// mock mode are expected):');
    errors.slice(0, 20).forEach((e) => console.log('  ' + e));
  }
})();
