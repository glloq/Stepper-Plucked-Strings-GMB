# Stepper-Plucked-Strings-GMB — Web configuration interface

Local, browser-based configuration UI for the ESP32-S3 MIDI instrument
controller. It lets a beginner set up and run the instrument entirely from a
phone, tablet or desktop, with no app to install and no source code to edit.

It implements the interface described in the project specs:

- `SPECIFICATION.md` — dashboard (§19), setup wizard (§10), configurable
  GPIO management (§11), motor/servo/note config (§12–15), MIDI parameters
  (§18), profile storage (§20), safety/panic (§21).
- `STRING_FRET_SELECTION.md` — explicit string/fret selection over MIDI CC,
  the General-Midi-Boop preset, the MIDI monitor and the integrated test tool.
- `SYSEX_CAPABILITIES.md` — GMB identity &
  capabilities page and the integrated SysEx tester.

## What it is

Vanilla **HTML / CSS / JavaScript** — no framework, no CDN, no build step. The
files are small and fully self-contained so they can be flashed to the ESP32's
**LittleFS** and served directly by the firmware's web server.

Scripts are plain classic `<script>` files sharing a global `GMB` namespace
(not ES modules), specifically so the page also works when opened straight from
disk (`file://`), where module imports would be blocked by the browser.

## How it is served

The firmware serves the static files from LittleFS at the device root:

```
/            -> index.html
/css/style.css
/js/*.js
/api/...     -> REST endpoints (below)
/ws/midi     -> WebSocket, live MIDI monitor stream
/ws/status   -> WebSocket, live dashboard/state stream
```

Reach it at the device IP (station mode) or the captive-portal address in
access-point mode (default SSID `Stepper-Plucked-Strings-GMB`).

## Mock mode (standalone / demo)

Every REST call tries `fetch()` first and, if it fails (no backend — e.g. you
opened `index.html` directly), transparently falls back to an in-memory mock
with realistic sample data: a **4-string GCEA ukulele**. The WebSocket streams
fall back to timed mock pumps that emit a plausible GMB tablature sequence and
live status jitter. A pulsing **DEMO / MOCK DATA** badge appears in the top bar
whenever mock data is in use.

This means you can open `web-interface/index.html` in any browser and exercise
the entire UI — wizard, pin grid, MIDI monitor, test tool, SysEx tester,
profiles — with nothing else running.

To try it:

```
# just open the file, or serve it locally:
python3 -m http.server -d web-interface 8080
# then browse http://localhost:8080/
```

## Structure

```
web-interface/
├── index.html            SPA shell + script load order
├── css/style.css         responsive styling, light/dark via prefers-color-scheme
├── js/
│   ├── api.js            REST + WebSocket client, board profile, mock backend
│   ├── app.js            shell, routing, DOM helpers, draft-profile state, mode toggle
│   ├── dashboard.js      dashboard (§19)
│   ├── pins.js           GPIO assignment grid (§11)
│   ├── wizard.js         9-step setup wizard (§10)
│   ├── midimonitor.js    reusable real-time MIDI monitor (§15)
│   ├── midiselect.js     MIDI page: string/fret selection (§14) + params (§18) + test tool (§16)
│   ├── sysex.js          GMB identity & capabilities + SysEx tester (§17/§18)
│   └── profiles.js       profile list/create/copy/rename/delete/export/import/restore (§20)
└── README.md
```

## Simplified vs Advanced mode

A toggle in the sidebar switches between **Simplified** (beginner: recommended
values, hidden fine-tuning, only recommended GPIOs) and **Advanced** (manual
GPIO assignment including caution pins, detailed motor/servo/homing parameters,
SysEx block toggles, raw byte views), per SPECIFICATION.md §9.2.

## Per-string servos, endstops & fret editor (wizard steps 5–7)

The setup wizard configures a full instrument (1–6 strings) with a stepper plus
servos per string, **with or without a PCA9685**:

- **Servos per string (step 6).** For each string, add the servos it uses —
  **finger**, **strum**, an optional **strum lift** (raises/lowers the strum
  servo per stroke), **damper** and an optional **pluck**. Each servo picks its
  signal **source**:
  - **PCA9685** — choose `pcaBoard` (0–3, i.e. up to four boards / 64 channels)
    and `channel` (0–15). A compact channel-availability map flags duplicate
    `board+channel` in red.
  - **Direct GPIO** — choose a free ESP32 pin, filtered with the same
    green/yellow/red capability rules as the pin grid (reserved/USB pins hidden,
    caution pins Advanced-only, pins already used by a stepper signal or another
    servo excluded).

  The system works with **no PCA at all** (every servo on a direct GPIO) or any
  mix. Per-string servos get their `stringIndex` set automatically; Advanced mode
  also exposes **shared/auxiliary** servos (`stringIndex = -1`, e.g.
  `sharedStrum`/`aux`). Each servo carries its calibration (rest/active µs,
  pulse min/max, inverted, travelMs, settleMs, disableAtRest) and **Test
  rest/active** buttons (`POST /api/test/servo`).

- **Endstops per string (step 5).** Each string's HOME switch GPIO
  (input+interrupt capable) plus the full homing sub-object
  (`sensorActiveHigh/direction/fast+slow speed/backoff/offset/timeout/maxSearch`),
  and an optional **LIMIT** switch GPIO (Advanced). A **Test endstop** button
  shows a live HIGH/LOW readout (`POST /api/test/endstop`).

- **Fret positions per string (step 7).** A per-string table with one row per
  fret (0..`maxFret`) editing `calibratedFretMm[]`: **Fill automatically
  (theoretical)** fills every fret from `scaleLength·(1−2^(−fret/12))`, per-fret
  **+/−** nudge buttons and a direct numeric field, **Go to this fret**
  (jog/test — updates the displayed motor position), and **Save
  position**. A calibrated value always overrides theory in the firmware.

## Backend endpoints

REST (all JSON):

| Method | Path | Purpose |
| ------ | ---- | ------- |
| GET  | `/api/status` | dashboard live state (§19) |
| GET  | `/api/profile` | active working profile |
| PUT  | `/api/profile` | validate + atomically activate a profile; returns new `capabilitiesRevision` |
| GET  | `/api/profiles` | list of saved profile slots |
| POST | `/api/profiles` | `{action, payload}` — create / copy / rename / delete / setStartup |
| GET  | `/api/board/{id}` | board pin capabilities (e.g. `esp32-s3-devkitc-1`) |
| POST | `/api/pins/auto` | automatic conflict-free pin assignment |
| POST | `/api/pins/validate` | validate assignments, returns per-signal errors + suggestions |
| POST | `/api/panic` | software panic / STOP (§21.3) |
| POST | `/api/test/note` | integrated note/string/fret test; returns a step trace (§16) |
| POST | `/api/test/servo` | drive one servo to `rest`/`active`; returns the pulse width + wiring |
| POST | `/api/test/endstop` | live HOME/LIMIT switch readout for one string (`{level, active}`) |
| POST | `/api/sysex/request` | run a SysEx request, returns sent + received + decoded (§18) |
| GET  | `/api/capabilities` | computed GMB capabilities snapshot (§17) |

`POST /api/test/servo` body: `{ function, source, pcaBoard, channel, gpio, to:"rest"|"active", restUs, activeUs }`.
`POST /api/test/endstop` body: `{ string, homeGpio, limitGpio, sensorActiveHigh }` → `{ home:{gpio,level,active}, limit?:{...} }`.

WebSocket:

| Path | Streams |
| ---- | ------- |
| `/ws/midi` | MIDI monitor events `{timeMs, channel, type, cc/note, value, interpretation}` |
| `/ws/status` | live dashboard/state snapshots (same shape as `GET /api/status`) |

## Profile JSON

Import/export use the project profile schema (`project`, `profileVersion`,
`capabilitiesRevision`, `instrument`, `board`, `pins`, `network`, `midi`,
`stringFretSelection`, `strings`, `servos`). Field names match the firmware core
(`firmware/src/core/…`). Each entry in `servos` carries
`source` (`"pca"`/`"gpio"`), `stringIndex`, `pcaBoard`, `channel` and `gpio`
alongside its µs calibration; each string in `strings` carries a `homing`
sub-object and an optional `calibratedFretMm[]` table.
**The Wi-Fi password is never included in exports.**

## Notes

- No actuator is driven in normal mode until critical validation errors are
  cleared; the wizard's Validation step and the pin validator surface these.
- Only a validated, activated profile is published over SysEx; a draft is never
  announced. Saving increments `capabilitiesRevision`.
