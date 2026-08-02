# Stepper-Plucked-Strings-GMB — Web configuration interface

Local, browser-based configuration UI for the ESP32-S3 MIDI instrument
controller. It lets a beginner set up and run the instrument entirely from a
phone, tablet or desktop, with no app to install and no source code to edit.

It implements the interface described in the project specs:

- `cahier des charges.md` — dashboard (§19), setup wizard (§10), configurable
  GPIO management (§11), motor/servo/note config (§12–15), MIDI parameters
  (§18), profile storage (§20), safety/panic (§21).
- `selection corde et frette.md` — explicit string/fret selection over MIDI CC,
  the General-Midi-Boop preset, the MIDI monitor and the integrated test tool.
- `Communication automatique des capacités par SysEx.md` — GMB identity &
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
SysEx block toggles, raw byte views), per cahier des charges §9.2.

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
| POST | `/api/sysex/request` | run a SysEx request, returns sent + received + decoded (§18) |
| GET  | `/api/capabilities` | computed GMB capabilities snapshot (§17) |

WebSocket:

| Path | Streams |
| ---- | ------- |
| `/ws/midi` | MIDI monitor events `{timeMs, channel, type, cc/note, value, interpretation}` |
| `/ws/status` | live dashboard/state snapshots (same shape as `GET /api/status`) |

## Profile JSON

Import/export use the project profile schema (`project`, `profileVersion`,
`capabilitiesRevision`, `instrument`, `board`, `pins`, `network`, `midi`,
`stringFretSelection`, `strings`, `servos`). Field names match the firmware core
(`firmware/src/core/…`). **The Wi-Fi password is never included in exports.**

## Notes

- No actuator is driven in normal mode until critical validation errors are
  cleared; the wizard's Validation step and the pin validator surface these.
- Only a validated, activated profile is published over SysEx; a draft is never
  announced. Saving increments `capabilitiesRevision`.
