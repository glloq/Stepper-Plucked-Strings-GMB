/*
 * screenshots.js — regenerate img/screenshots/*.png from the real interface.
 *
 * The UI falls back to its in-memory mock backend when no device answers (see
 * js/api.js), so opening index.html from file:// gives a complete, deterministic
 * demo instrument — a 4-string GCEA ukulele, with a geared finger on string 1 —
 * and every screenshot in the documentation is taken from it.
 *
 * Usage (Playwright is NOT a project dependency — install it wherever you like):
 *
 *   npm i playwright && npx playwright install chromium
 *   node web-interface/tools/screenshots.js
 *
 * Set CHROMIUM_PATH to use an already-installed Chromium instead.
 *
 * Each page is shot full-height with the viewport fitted to the rendered content,
 * so the images carry no band of empty background. Keep the file names stable:
 * they are referenced from README.md, README_EN.md and docs/WEB_INTERFACE.md.
 */
'use strict';

const { chromium } = require('playwright');
const path = require('path');

const ROOT = path.resolve(__dirname, '..', '..');
const OUT = path.join(ROOT, 'img', 'screenshots');
const URL = 'file://' + path.join(ROOT, 'web-interface', 'index.html');
const WIDTH = 1400;                    // desktop layout, single column of cards
const SCALE = 1.5;                     // crisp text without 4 MB PNGs

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

(async () => {
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
  await sleep(600);

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

  const view = async (id) => { await page.evaluate((v) => GMB.navigate(v), id); await sleep(500); };
  const step = async (id) => { await page.evaluate((s) => GMB.gotoSetupStep(s), id); await sleep(600); };
  const sub = async (label) => { await page.click('.subtab:text-is("' + label + '")'); await sleep(600); };

  console.log('Instrument');
  await view('fretboard');
  await shot('fretboard');

  console.log('Setup');
  await step('builder');
  await shot('wizard');

  await step('frets');
  // Open the first equipped fret so the servo editor is part of the picture.
  const chip = page.locator('.fret-chip.equipped, .fret-chip.geared').first();
  if (await chip.count()) { await chip.click(); await sleep(500); }
  await shot('calibration');

  await step('plucking');
  await shot('calibration-plucking');
  await step('midi');
  await shot('midi');
  await step('power');
  await shot('power');
  await step('test');
  await shot('calibration-test');
  await step('validation');
  await shot('validation');

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
  await sleep(800);
  await shot('network', { fit: false });
  await page.click('.settings-tab:text-is("Advanced")');
  await sleep(1200);
  await shot('settings-advanced', { fit: false });

  // The Advanced tab scrolls; bring each card into view for its own capture.
  async function scrollTo(heading) {
    await page.evaluate((t) => {
      const body = document.getElementById('settings-body');
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
