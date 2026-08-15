# Device vs Instrument separation (audit P1.13)

A profile historically mixed two concerns:

- **Device config** — which ESP32 board, the Wi-Fi/network settings, the system
  GPIO pin map, the E-stop contact wiring and the declared power/safety hardware.
  This belongs to the *physical machine* and does not change when you play a
  different tune.
- **Instrument profile** — instrument identity, MIDI mapping, string/fret selector,
  power governor, plucking gesture, the axes (transmission, travel, fret geometry),
  their **homing configuration**, and the servos. This is *portable*: the same
  instrument definition should load onto any compatible device.

  Homing sits on the instrument side deliberately. A homing config describes the
  *carriages* — seek direction and speeds, back-off, the rest offset from the HOME
  sensor, and each endstop's polarity — not the controller board. Carrying an
  instrument to another ESP32 has to bring those references with it, or the first
  re-home on the new device would seek against the wrong reference.

The goal of P1.13 is to make that boundary explicit so the two can evolve — and
eventually be stored and swapped — independently.

## What is in place

### 1. The two structs + a lossless split/merge (host-tested)

`core/configuration/DeviceInstrument.h` defines `DeviceConfig` and `InstrumentProfile`
and the pure functions `deviceConfigOf()`, `instrumentProfileOf()` and
`mergeProfile()`. These split a combined `Profile` into its two halves and recombine
them without loss. Covered by `firmware/test/test_device_instrument.cpp`.

### 2. The split is persisted on disk (this step)

Stored profile **slots** now use a split on-disk layout, while the **interchange
format** used by the web API and by import/export stays flat and unchanged. Two thin
re-parenting wrappers in `ProfileStorage` bridge them, so there is exactly one set of
field (de)serialisers — no duplicated logic:

| Path | Format | Serialiser |
| ---- | ------ | ---------- |
| Web `GET/PUT /api/profile`, import/export | **flat** (unchanged) | `toJson` / `fromJson` |
| On-disk slot files (LittleFS) | **split** | `toSlotJson` / `fromSlotJson` |

On-disk slot shape:

```json
{
  "storageFormat": "gmb-split-v1",
  "project": "...",
  "profileVersion": 2,
  "capabilitiesRevision": 7,
  "device":     { "board": {...}, "pins": [...], "network": {...} },
  "instrument": { "info": {...}, "midi": {...}, "stringFretSelection": {...},
                  "power": {...}, "pluck": {...}, "strings": [...], "servos": [...] }
}
```

`storageFormat` marks the on-disk *layout* generation. It is deliberately **orthogonal
to `profileVersion`**, which continues to version the field *schema* shared with the
flat interchange format. Keeping them separate lets the storage layout change without
disturbing the web contract (the web keeps consuming the flat form at its own version).

### Migration — no stored profile is orphaned

`fromSlotJson` reads a split slot *or* a legacy flat slot (one written before the
split, including an old v1 flat slot, which it migrates via the existing `migrate()`
step). A legacy slot is therefore loaded unchanged and **rewritten in the split form on
the next save** — a lazy, non-destructive migration. The atomic temp-file + `.bak`
save/restore path is unchanged, so a power loss mid-migration never loses a profile.

Covered by `firmware/test/profilecheck` ("device/instrument split slot storage"):
the split shape is asserted, a profile round-trips losslessly through it, and both a
legacy flat slot and a legacy v1 flat slot still load.

## What is deferred (not yet done — honest status)

- ~~**Behavioural portability.**~~ **Done.** The distinction is not *which fields*
  but *which operation*:

  | Operation | What it means | What is taken |
  | --------- | ------------- | ------------- |
  | `PUT /api/profile` | publish the draft you just edited **for this machine** | the whole profile — pins, board and network included, because you meant them |
  | `POST /api/profiles/load` | load a stored **instrument** onto this machine | the instrument half only; the device half of the RUNNING config survives |

  `onActivateProfile(profile, keepDeviceConfig)` carries the distinction, and the
  merge uses `mergeProfile(deviceConfigOf(running), instrumentProfileOf(loaded))`.
  The MERGED profile is what gets validated, so the instrument has to fit *this*
  device's pins — which is the combination that will actually run.

  This was not cosmetic. Adopting a slot's device half replaced the running
  machine's network settings (the radio was not re-initialised, so `/api/status`
  then reported a network the device was not on), its pin map, the declared power
  hardware, and — worst — `estopNormallyClosed`, the E-stop polarity. Importing a
  safety wiring declaration from a file saved on another machine is exactly the
  kind of thing that must not happen quietly.
- **Web/interchange split.** The web UI consumes the flat profile shape at ~100 call
  sites. Restructuring the interchange format would ripple through the browser UI,
  which cannot be functionally validated in the software-only phase — so it is left for
  the bench/browser phase. The on-disk split above needs none of that.
- **`MidiTransportConfig` / `SafetyConfig`** will join `DeviceConfig` as those features
  gain a persisted shape (P1.7 / P1.10 groundwork).

> As with every mechanical item in this repository, only the software behaviour above
> is validated (host tests + sanitizers + ESP32 CI build). Nothing here has been
> exercised on a powered device.


---

## Ce qui démarre : l'instantané actif (`/active.json`)

Les 8 slots sont une **bibliothèque**. Ils ne sont pas la configuration qui
tourne, et ils étaient le mauvais endroit d'où démarrer : un slot embarque une
moitié *device* (carte, broches, câblage E-stop, matériel monté) figée au moment
où il a été écrit — donc recâbler la machine puis publier fonctionnait pour la
session et revenait silencieusement en arrière au redémarrage suivant.

Un seul fichier porte la vérité :

```text
/active.json
├── device      ← cette machine (carte, broches, E-stop, matériel, réseau)
└── instrument  ← ce qui joue (cordes, homing, servos, MIDI, pluck)
```

### Pourquoi un fichier et pas deux

Il y en a eu deux, `/device.json` et `/current.json`, chacun écrit
atomiquement. **La paire ne l'était pas** : le premier pouvait réussir et le
second échouer, laissant le boot suivant reconstruire une nouvelle config
machine avec un ancien instrument — une combinaison qui n'a jamais existé et n'a
jamais été validée. Un fichier a un seul point de commit ; cet état devient
irreprésentable.

### Persister et activer, ensemble ou pas du tout

Écrire puis mettre en file, ou l'inverse, ne satisfait ni l'un ni l'autre :

| Ordre | Ce qui casse |
| ----- | ------------ |
| persister → activer | file pleine ⇒ HTTP 503 alors que le flash contient déjà la nouvelle configuration : la requête a échoué et le prochain boot a changé |
| activer → persister | échec flash ⇒ la machine tourne sur une configuration qu'elle ne retrouvera pas, et l'UI a déjà annoncé « publié et ACTIF » |

L'écriture est donc coupée à son point de commit :

```text
prepareActive()   écrit + relit un fichier temporaire — rien de visible n'a changé
   ↓
enqueue()         l'activation est acceptée, ou refusée
   ↓
commitActive()    un seul rename       │   discardActive()   le temporaire disparaît
(accepté)                              │   (refusé : le stockage est intact)
```

Si l'écriture est impossible, `PUT /api/profile` répond **507** et **n'active
rien** : faire tourner une configuration qui n'a pas pu être écrite est
exactement l'ambiguïté que ce modèle existe pour supprimer.

### Absent n'est pas corrompu

Au boot, `loadActive()` distingue trois cas :

| État | Comportement |
| ---- | ------------ |
| `Ok` | démarrage normal |
| `Missing` | premier boot ou installation antérieure : lecture des anciens emplacements, puis migration écrite une fois |
| `Unreadable` | **CONFIG_SAFE**, avec le fichier fautif nommé dans le journal |

Se rabattre sur un autre profil parce que le fichier de la machine ne se lit
plus, ce serait piloter ces chariots avec la carte de broches de quelqu'un
d'autre. La récupération `.bak` de `begin()` couvre aussi ce fichier — c'est
celui qui est réécrit à chaque publication, donc le plus exposé à une coupure
pendant le rename.
