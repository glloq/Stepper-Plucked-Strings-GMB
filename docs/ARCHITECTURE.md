# Architecture — Stepper-Plucked-Strings-GMB

> Document de référence : [`CAHIER_DES_CHARGES.md`](CAHIER_DES_CHARGES.md) (source : `cahier des charges.md` §23, §24).
> Documents liés : [`PIN_CONFIGURATION.md`](PIN_CONFIGURATION.md) · [`MIDI_PROTOCOL.md`](MIDI_PROTOCOL.md) · [`WEB_INTERFACE.md`](WEB_INTERFACE.md) · [`CALIBRATION.md`](CALIBRATION.md) · [`SAFETY.md`](SAFETY.md)

Ce document décrit l'organisation logicielle du firmware, la façon dont les
modules du cahier des charges (§23) correspondent au code réellement implémenté
dans `firmware/src/core/*`, le flux de données de bout en bout, la génération du
snapshot de capacités, la stratégie « cœur pur + adaptateurs de plateforme +
tests natifs », et les phases de développement (§24).

---

## 1. Stratégie : cœur pur, adaptateurs de plateforme, tests natifs

Le cœur algorithmique du firmware est écrit en **C++17 pur**, sans aucune
dépendance Arduino ou ESP-IDF. Cette contrainte est explicite dans le code :

```cpp
// Types.h — This header is pure C++17 with no Arduino / ESP-IDF dependency so
// that the whole algorithmic core can be unit-tested natively on a host with g++.
```

Conséquences :

* **Cœur pur** (`firmware/src/core/`) — logique métier testable sur PC : profils
  de cartes, gestion des broches, sélection corde/frette, allocation des notes,
  machine d'état par corde, géométrie moteur, homing, capacités/SysEx GMB,
  sécurité. Aucun accès direct au matériel.
* **Adaptateurs de plateforme** (`firmware/src/platform/esp32/`) —
  implémentations concrètes qui branchent le cœur sur le matériel ESP32-S3 :
  `StepperBank` génère les pas via le **moteur matériel FastAccelStepper**
  (RMT/MCPWM + timer), donc **hors de `loop()`** — le `MotionPlanner` du cœur
  reste le modèle trapézoïdal de référence testé sur PC ; `ServoBank` pilote le
  PCA9685 **et** les servos en GPIO direct (LEDC 14 bits) ; `Net` (Wi-Fi non
  bloquant), `WebApi` (REST + WebSocket), `MidiWifi` (transport), `ProfileStorage`
  (LittleFS + NVS pour les secrets). Ces couches consomment le cœur sans le
  modifier.
* **Tests natifs** (`firmware/test/`) — 86 tests unitaires compilés et exécutés
  avec `g++ -std=c++17` via `firmware/test/Makefile`, couvrant les 8 modules du
  cœur (`test_board`, `test_selector`, `test_allocator`, `test_motion`,
  `test_string_fsm`, `test_profile`, `test_sysex`, + `test_main`).

```bash
cd firmware/test && make        # compile le cœur + les tests, puis les exécute
```

Cette séparation garantit qu'un nouveau transport MIDI ou une nouvelle carte
n'affecte pas le contrôleur des cordes, l'allocateur, la gestion des mouvements
ni les profils mécaniques (cahier des charges §8.3).

---

## 2. Arborescence cible (§23) et correspondance avec le code

Le cahier des charges §23 décrit l'arborescence **cible** complète. Le tableau
ci-dessous met en regard cette arborescence avec les modules **effectivement
implémentés** dans `firmware/src/core/`.

```text
firmware/                        Cahier des charges §23        Implémenté (core/)
├── application/
│   ├── Application              orchestration                 (adaptateur, à venir)
│   ├── Scheduler                planification non bloquante    (adaptateur, à venir)
│   └── EventBus                 bus d'événements               (adaptateur, à venir)
├── board/
│   ├── BoardProfile             profils de cartes             core/board/BoardProfile.{h,cpp}
│   ├── PinManager               attribution des broches       core/board/PinManager.{h,cpp}
│   └── PinValidator             validation des conflits       PinManager::validate() (fusionné)
├── communication/
│   ├── WifiManager              Wi-Fi AP/station              (adaptateur, à venir)
│   ├── MidiTransport            transport MIDI                (adaptateur, à venir)
│   ├── WebSocketMidi            MIDI sur WebSocket            (adaptateur, à venir)
│   └── FutureTransports         BLE/USB/DIN…                  MidiSource enum (core/midi)
├── midi/
│   ├── MidiParser               parsing octets → MidiEvent    core/midi/MidiEvent.h
│   ├── MidiRouter               routage                       (adaptateur, à venir)
│   └── MidiEventQueue           file d'événements             (adaptateur, à venir)
│   └── (sélection corde/frette) tablature CC20/CC21           core/midi/StringFretSelector.{h,cpp}
├── instrument/
│   ├── InstrumentController     orchestration instrument      (adaptateur, à venir)
│   ├── StringController         machine d'état par corde      core/instrument/StringController.{h,cpp}
│   ├── NoteAllocator            allocation des notes          core/instrument/NoteAllocator.{h,cpp}
│   └── SharedStrummer           grattage partagé              (phase 4, à venir)
├── motion/
│   ├── StepperAxis              géométrie/conversion mm↔pas   core/motion/StepperAxis.{h,cpp}
│   ├── MotionPlanner            profil trapézoïdal (accel)    core/motion/MotionPlanner.{h,cpp}
│   └── HomingController         homing non bloquant           core/motion/HomingController.{h,cpp}
├── actuators/
│   ├── ServoManager             PCA9685                       ServoConfig (core/configuration)
│   ├── FingerActuator           servo de doigt                ServoConfig function="finger"
│   ├── PluckActuator            servo de pincement            ServoConfig function="pluck"
│   └── DamperActuator           étouffoir                     ServoConfig function="damper"
├── configuration/
│   ├── Profile                  profil (source de vérité)     core/configuration/Profile.{h,cpp}
│   ├── ProfileValidator         validation                    core/configuration/ProfileValidator.{h,cpp}
│   └── ProfileStorage           persistance NVS               (adaptateur, à venir)
├── safety/
│   ├── SafetyManager            états sûrs / panic / E-stop   core/safety/SafetyManager.{h,cpp}
│   └── FaultManager             journal des défauts           SafetyManager::faults() (fusionné)
├── diagnostics/
│   ├── Logger                   journalisation                (adaptateur, à venir)
│   └── DiagnosticService        diagnostics                   (adaptateur, à venir)
├── gmb/                         (protocole SysEx GMB, §ci-dessous)
│   ├── Capabilities             snapshot de capacités         core/gmb/Capabilities.{h,cpp}
│   └── GmbSysEx                 encodeur/décodeur SysEx       core/gmb/GmbSysEx.{h,cpp}
└── web/
    ├── WebServer                serveur HTTP                  (adaptateur, à venir)
    ├── RestApi                  API REST                      (adaptateur, cf. WEB_INTERFACE.md)
    └── WebSocketStatus          statut temps réel             (adaptateur, à venir)
```

Notes de correspondance :

* `PinValidator` (§23) est fusionné dans `PinManager::validate()` — la validation
  et l'attribution partagent le même `BoardProfile`.
* `FaultManager` (§23) est fusionné dans `SafetyManager` (`recordFault()` /
  `faults()`).
* Le module `gmb/` n'apparaît pas explicitement dans l'arbre §23 : il matérialise
  la spécification [`Communication automatique des capacités par SysEx.md`](CAHIER_DES_CHARGES.md).
* Les entrées marquées « adaptateur, à venir » sont des couches de plateforme ou
  des modules de phases ultérieures qui consommeront le cœur.

---

## 3. Flux de données principal

Un transport MIDI produit un `MidiEvent` interne unique (cahier des charges §8.2),
et tout le reste du firmware ne dépend jamais de la manière dont les octets sont
arrivés.

```text
Transport (WebSocket / RTP-MIDI / UDP / test Web / futur BLE/USB/DIN)
        │  décodage
        ▼
MidiEvent { timestampUs, source, type, channel, data1, data2 }
        │
        ├──► SysEx (F0 …) ─────────────► GmbSysEx  ──► CapabilitySnapshot ──► réponse
        │
        ▼
MidiRouter (routage par canal / Omni)
        │
        ▼
StringFretSelector          (sélection corde/frette explicite CC20/CC21, FIFO)
   ├─ onControlChange()      empile les sélections corde/frette en attente
   ├─ onNoteOn() ──► NoteResolution { play, source, stringIndex, fret, instanceId }
   └─ onNoteOff() ──► ActiveNote (relâche la corde réellement utilisée)
        │
        │  (mode Automatique / Hybride sans CC valide)
        ▼
NoteAllocator               (choisit la meilleure corde, regroupe les accords,
                             applique la stratégie de saturation)
        │  Allocation { stringIndex, fret }
        ▼
StringController[c]          (machine d'état non bloquante, 1 par corde)
   DISABLED → HOMING → IDLE → RELEASING_FINGER → MOVING →
   PRESSING_FINGER → SETTLING → READY_TO_PLUCK → PLUCKING →
   SUSTAINING → DAMPING (→ IDLE)     |  CANCELLING  |  FAULT
        │                                   │
        ▼                                   ▼
StepperAxis / HomingController        ServoManager (PCA9685)
   (mm ↔ pas, positions de frettes)      doigt / pincement / étouffoir
```

Points clés du flux :

* **Événement commun.** `MidiEvent` (voir [`MIDI_PROTOCOL.md`](MIDI_PROTOCOL.md))
  gère les subtilités MIDI (`isNoteOff()` traite un Note On vélocité 0 comme un
  Note Off à running status).
* **Sélection avant allocation.** En mode `Explicit`/`Hybrid`, `StringFretSelector`
  impose la corde/frette ; en mode `Automatic` ou en repli, `NoteAllocator`
  décide. Détails dans [`MIDI_PROTOCOL.md`](MIDI_PROTOCOL.md).
* **Identifiant de commande.** Chaque `noteOn(fret)` renvoie un `commandId` frais ;
  toute action différée taguée avec un ancien id est ignorée. Cela empêche un
  pincement après Note Off, un appui retardé, l'exécution d'une position obsolète
  ou une attaque après un panic (cahier des charges §16).
* **Note Off fiable.** L'affectation réelle d'un Note On est mémorisée
  (`ActiveNote`) pour relâcher la bonne corde, même en accord ou notes répétées.

---

## 4. Flux du snapshot de capacités (SysEx GMB)

Le profil actif est **l'unique source de vérité**. Les capacités annoncées à
General-Midi-Boop sont reconstruites depuis ce profil, jamais codées en dur.

```text
Interface Web édite un brouillon
        │
        ▼
ProfileValidator (validation complète)
        │  valide
        ▼
Sauvegarde atomique + incrément capabilitiesRevision
        │
        ▼
buildSnapshot(Profile) ──► CapabilitySnapshot (immuable)
        │   { revision, identity, descriptor, capabilities, stringConfig, valid }
        ▼
GmbSysEx::respond(request, snapshot)   (une réponse = un seul snapshot)
        │
        ▼
Transport MIDI ──► General-Midi-Boop actualise l'instrument
        ▲
        └── Bloc 8 (notification) invite GMB à relancer la découverte
```

`buildSnapshot()` (`core/gmb/Capabilities.cpp`) calcule automatiquement la plage
jouable (union des notes de toutes les cordes actives), le mode continu ou notes
discrètes, la polyphonie (nombre de cordes actives ou surcharge), et la liste des
CC réellement activés. Un snapshot est **immuable** : une modification de config
pendant l'envoi ne peut pas mélanger deux versions du profil. Voir le protocole
complet dans [`MIDI_PROTOCOL.md`](MIDI_PROTOCOL.md#3-protocole-sysex-gmb).

---

## 5. Le profil, colonne vertébrale de la configuration

`core/configuration/Profile.h` agrège toute la configuration :

| Champ | Type | Rôle |
| ----- | ---- | ---- |
| `instrument` | `InstrumentInfo` | nom, type, programme GM, nb cordes, capo, transposition, mode de pincement |
| `boardIdentifier` / `reserveUsb` / `pins` | — | carte, réservation USB, attribution GPIO |
| `network` | `NetworkConfig` | mode AP/station, SSID, hostname, IP fixe |
| `midi` | `MidiConfig` | canal, Omni, transposition, fenêtre d'accord, courbe de vélocité, pédale |
| `selector` | `SelectorConfig` | sélection corde/frette (CC20/CC21, mode, timeout, FIFO…) |
| `strings` | `vector<AxisConfig>` | géométrie/moteur par corde |
| `homing` | `vector<HomingConfig>` | homing par axe |
| `servos` | `vector<ServoConfig>` | servos (doigt/pincement/étouffoir/aux) |
| `capabilitiesRevision` | `uint32_t` | compteur de révision (notification Bloc 8) |

`Profile::instrumentView()` en dérive une `InstrumentView` partagée par le
sélecteur corde/frette et le générateur de capacités.

---

## 6. Phases de développement (§24)

| Phase | Objet | Livrables clés |
| ----- | ----- | -------------- |
| **1 — Prototype une corde** | ESP32-S3, Wi-Fi, UI minimale, 1 moteur, 1 capteur HOME, 1 servo doigt, 1 servo pincement, test MIDI Wi-Fi, machine d'état complète, panic | machine d'état, homing, panic |
| **2 — Configuration intuitive** | assistant, profil de carte, attribution auto des GPIO, validation des conflits, calibration moteur/servos, import/export JSON | `BoardProfile`, `PinManager`, `Profile`, wizard |
| **3 — Multicorde** | 4 puis 6 axes, PCA9685, homing parallèle, allocation des notes, accords, diagnostics par corde | `NoteAllocator`, homing parallèle |
| **4 — Jeu avancé** | grattage partagé, tremolo, étouffement, pédale de maintien, courbes de vélocité, stratégies de saturation | `SharedStrummer`, courbes |
| **5 — Matériel dédié** | schéma, PCB, protections, connecteurs, arrêt matériel, validation électrique, doc câblage | `hardware/` |
| **6 — Communications futures** | BLE MIDI, USB MIDI, MIDI DIN, liaisons filaires | nouveaux transports réutilisant `MidiEvent` |

L'état actuel du dépôt couvre le **cœur algorithmique** des phases 1 à 3 (modules
`core/*` + 86 tests natifs). Les adaptateurs de plateforme et l'interface Web
constituent les couches restantes.

---

## 7. Indépendance du transport

Ajouter un transport (BLE, USB, DIN, série, CAN/RS485) ne doit modifier ni le
contrôleur des cordes, ni l'allocateur, ni la gestion des mouvements, ni les
profils mécaniques (§8.3). Tous les transports :

1. décodent les octets en `MidiEvent` ;
2. transmettent des octets MIDI complets au routeur ;
3. réutilisent exactement les mêmes blocs, encodeur, décodeur, snapshot et tests
   pour le SysEx GMB (spec SysEx §21).

Les GPIO19/GPIO20 restent réservés par défaut pour l'USB natif ESP32-S3.
