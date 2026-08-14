#!/usr/bin/env node
/*
 * smoke.js — open every page of the interface in a real browser and fail on any
 * page that threw.
 *
 * `node --check` proves a file parses. `run-tests.js` proves the pure kernels
 * compute the right numbers. NEITHER can catch a view that calls a function that
 * does not exist — the render throws, the shell shows a "View error" card, and
 * every automated check still passes. That is exactly how the Power & safety tab
 * shipped broken (`GMB.ensureHardware is not a function`): it was only caught by
 * a human looking at a screenshot.
 *
 * So: mount every view and sub-tab, and assert that (a) nothing threw and (b) no
 * "View error" card is on screen. It needs a browser, so it is not part of the
 * default CI job — run it before publishing screenshots or touching a view:
 *
 *   npm i playwright && npx playwright install chromium
 *   node web-interface/test/smoke.js
 *
 * PLAYWRIGHT_MODULE / CHROMIUM_PATH work as in tools/screenshots.js.
 */
'use strict';

const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const path = require('path');

const ROOT = path.resolve(__dirname, '..', '..');
const URL = 'file://' + path.join(ROOT, 'web-interface', 'index.html');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let failures = 0;
function fail(where, what) {
  failures++;
  console.error('  [FAIL] ' + where + ' — ' + what);
}

(async () => {
  const browser = await chromium.launch(
    process.env.CHROMIUM_PATH ? { executablePath: process.env.CHROMIUM_PATH } : {});
  const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });

  // Page errors are attributed to whatever we were mounting when they landed.
  let where = 'load';
  const thrown = [];
  page.on('pageerror', (e) => thrown.push({ where, message: String(e) }));

  await page.goto(URL);
  await page.waitForSelector('.nav-item');
  await sleep(600);

  // The shell catches a render throw and paints a "View error" card rather than a
  // blank page — good for the user, invisible to a naive check. Look for it.
  async function assertClean(label) {
    await sleep(450);
    const errCard = await page.evaluate(() => {
      const el = Array.from(document.querySelectorAll('h2'))
        .find((h) => h.textContent.trim() === 'View error');
      return el ? el.parentElement.innerText.slice(0, 400) : null;
    });
    if (errCard) fail(label, 'rendered a "View error" card:\n      ' +
                             errCard.replace(/\n/g, '\n      '));
    const mine = thrown.filter((t) => t.where === label);
    mine.forEach((t) => fail(label, 'threw: ' + t.message));
    if (!errCard && !mine.length) console.log('  [ OK ] ' + label);
  }

  async function view(id, label) {
    where = label;
    await page.evaluate((v) => GMB.navigate(v), id);
    await assertClean(label);
  }
  async function sub(text, label) {
    where = label;
    await page.click('.subtab:text-is("' + text + '")');
    await assertClean(label);
  }
  async function settingsTab(text, label) {
    where = label;
    await page.click('.settings-tab:text-is("' + text + '")');
    await sleep(900);                       // some tabs fetch before painting
    await assertClean(label);
  }

  console.log('Main pages');
  await view('fretboard', 'Instrument');
  await view('hardware', 'Wiring & GPIO');
  await view('wizard', 'Setup');

  console.log('Setup steps');
  const steps = await page.evaluate(() => GMB.views.wizard.steps());
  for (const label of steps) {
    where = 'Setup › ' + label;
    await page.evaluate((s) => {
      if (GMB.state.current !== 'wizard') GMB.navigate('wizard');
      return GMB.views.wizard.goto(s);
    }, label);
    await assertClean('Setup › ' + label);
  }
  // Per-string steps render one string at a time; string 2 exercises the index
  // paths a first-string-only pass would miss.
  where = 'Setup › Mechanics (string 2)';
  await page.evaluate(() => {
    GMB.views.wizard.goto('Mechanics');
    GMB.views.wizard.selectString(1);
  });
  await assertClean('Setup › Mechanics (string 2)');

  console.log('Wiring & GPIO sub-tabs');
  await view('hardware', 'Wiring & GPIO (remount)');
  for (const [text, label] of [
    ['Harness', 'Wiring › Harness'],
    ['Power & safety', 'Wiring › Power & safety'],
    ['I²C & PCA', 'Wiring › I²C & PCA'],
    ['GPIO pins', 'Wiring › GPIO pins'],
    ['Commissioning', 'Wiring › Commissioning'],
  ]) await sub(text, label);

  console.log('Settings modal');
  where = 'Settings (open)';
  await page.evaluate(() => GMB.openSettings('profiles'));
  await sleep(900);
  await assertClean('Settings › Profiles');
  await settingsTab('Network', 'Settings › Network');
  await settingsTab('Security', 'Settings › Security');
  await settingsTab('Diagnostics', 'Settings › Diagnostics');
  await settingsTab('Tools', 'Settings › Tools');
  where = 'Settings (close)';
  await page.evaluate(() => GMB.closeSettings());
  await sleep(400);

  // Anything that threw outside a labelled step still counts.
  const stray = thrown.filter((t) => t.where === 'load' || t.where === 'Settings (open)' ||
                                     t.where === 'Settings (close)');
  stray.forEach((t) => fail(t.where, 'threw: ' + t.message));

  await browser.close();
  console.log('\n' + (failures ? failures + ' failure(s)' : 'smoke OK — every view rendered'));
  process.exit(failures ? 1 : 0);
})();
