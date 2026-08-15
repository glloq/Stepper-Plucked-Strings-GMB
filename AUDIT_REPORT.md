# Rapport de reprise — Stepper-Plucked-Strings-GMB

Report des améliorations menées sur **Servo-Plucked-Strings-GMB** vers ce dépôt,
**adaptées à la partie moteurs pas-à-pas**. Le dépôt servo a servi de base et
d'exemple ; chaque point a été repris, puis re-jugé pour une machine où la
frette est choisie par un **chariot**, pas par un servo dédié.

> ⚠️ **Aucune validation mécanique/électrique réelle** n'a été faite : tout ce
> qui suit est validé *en logiciel* (tests natifs, sanitizers, harnais runtime,
> compilation ESP32 en CI). La validation sur instrument sous tension reste à
> faire — commencer par [`hardware/COMMISSIONING.md`](hardware/COMMISSIONING.md).

---

## 1. Tableau de synthèse

| # | Point | Statut | Adaptation pas-à-pas |
| - | ----- | ------ | -------------------- |
| P0.1 | Supprimer l'auto-armement d'un profil de secours → CONFIG_SAFE | **DONE** | identique |
| P0.2 | Vraie phase d'armement gatée avant tout jeu | **DONE** | `Parking` (timer) → `Homing` (**capteur**) : `beginHoming`/`releaseComplete`/`armAfterHoming` |
| P0.3 | Séparer `hardStop()` et le park contrôlé | **DONE** | + `StepperBank::hardStop()` / `controlledStopAll()` / `stopDurationMs()` |
| P0 | Park **gouverné** à l'armement (pas de /OE sur des sorties latchées) | **DONE** | un park refusé **abandonne le homing** (un doigt encore posé ne doit jamais voir un chariot partir) |
| P1.4 | API homogène `ActuatorResult` | **DONE** | étendu à `StepperBank::moveToMm/moveToMmRaw/setVelocityMm` |
| P1.5 | Perte PCA locale (faute des cordes concernées, pas de panic global) | **DONE** | identique |
| P1.6 | PowerGovernor sur tous les mouvements | **DONE** | le **repositionnement de chariot** est *staggerable* (2ᵉ source d'appel de courant) |
| P1.7 | Abstraction des transports MIDI | **DONE** | UDP + DIN branchés (UART2, broche `MIDI_RX`) ; USB-MIDI implémenté dans l'env optionnel `esp32-s3-usbmidi`, **non validé sur matériel** |
| P1.9 | `staticIp` : retiré (option fantôme) | **DONE** | identique |
| P1.10 | `WifiLossBehavior` : retiré (jamais câblé) | **DONE** | identique |
| P1.11 | Postures réseau + gate UDP | **DONE** | `UdpSourceGate` host-testé + `POST /api/midi/source` (politique persistée en NVS, appliquée par la boucle) |
| P1.12 | Migration de profils v1→v2 + fixture | **DONE** | identique |
| P1.13 | Split `DeviceConfig`/`InstrumentProfile` + slots disque | **DONE** | le **homing** voyage côté *instrument* (§3) ; persistance fermée : `/device.json` (la machine) + `/current.json` (l'instrument qui tourne), le boot ne dépend plus du slot |
| P1.14 | Builds PlatformIO multi-cartes | **DONE** | 3 cartes + note sur les unités RMT/MCPWM |
| P1.15 | Réserver GPIO0 (BOOT-hotspot) | **DONE** | identique, testé sur les 4 profils de carte |
| P1.16 | CI GitHub Actions complète | **DONE** | + `stepperbankcheck` + `boardcheck` |
| P2.17 | Réduire `main.cpp` (kernels extraits) | **DONE** | 1903 → ~1250 lignes ; `PlaybackScheduler` / `SafetySupervisor` / `CommandDispatcher` extraits (corps déplacés à l'identique) et couverts par `runtimecheck` |
| P2.18 | Documenter les hypothèses du modèle | **DONE** | + la loi de **géométrie** propre à ce build |
| P2.19 | Télémétrie / `GET /api/diagnostics` | **DONE** | + compteurs mouvement (axes, homing, LIMIT, timeouts) |

**18 DONE** — les quatre PARTIAL de la première reprise ont été fermés depuis
(transports MIDI, gate UDP, split Device/Instrument, découpe de `main.cpp`).

Une réserve subsiste et n'est pas un PARTIAL déguisé : **USB-MIDI compile et n'a
jamais énuméré face à un hôte**. Il est derrière un env dédié pour cette raison,
et l'image S3 par défaut ne change pas.

---

## 2. Résultats des vérifications

| Vérification | Résultat |
| ------------ | -------- |
| Tests natifs cœur (`-Wall -Wextra -Werror`, désormais le défaut local) | **256 tests, 5484 checks, 0 failures** |
| Idem sous **AddressSanitizer + UBSan** | **256 tests, 0 failures** |
| `hostcheck` (compile `main.cpp` + adaptateurs ESP32) | 7/7 unités OK |
| `servobankcheck` (2 bus, park contrôlé + gouverné, `ActuatorResult`, mapping P1.5) | OK |
| `stepperbankcheck` (**nouveau** : hardStop vs stop contrôlé, moves refusés, butées logicielles, générateur de pas absent) | OK |
| `boardcheck` (les JSON de cartes correspondent à `BoardProfile.cpp`) | OK |
| `runtimecheck` (**nouveau** : `SafetySupervisor` 11 cas + `PlaybackScheduler` 6 cas, contre les vraies banques) | OK |
| `apicheck` (**contrat** : 32 endpoints — méthodes, champs de requête lus, champs de réponse écrits) | OK |
| `profilecheck` (5 profils + migration v1→v2 + slots split/hérités) | OK |
| Tests JS web + rendu navigateur (Playwright) | OK — toutes les vues rendues |
| Builds ESP32 (S3 / WROOM-32 / DevKit v1) | en CI (le toolchain xtensa n'est pas téléchargeable dans le bac à sable) |

Tests natifs : 147 → **256**, plus six harnais runtime, le contrat REST et le
test navigateur.

> Ce tableau est daté par nature. La CI est la source de vérité : si un chiffre
> ici diverge d'elle, c'est ce fichier qui a vieilli.

---

## 3. Les points où l'adaptation change la réponse

La plupart des points se transposent tels quels. Quatre méritent d'être
explicités, parce que la version servo aurait été **fausse** ici.

### P0.2 — `Parking` (minuteur) → `Homing` (capteur)

Le build servo arme après `max(travel+settle)` : un park de servos est un pur
minuteur. Ici, l'armement doit **établir une référence physique** — chaque axe
cherche son capteur HOME — ce qui n'est pas borné par un délai connu. La FSM
expose donc `beginHoming()` puis `armAfterHoming()`, commandé par l'appelant
quand tous les axes activés sont ancrés. Un seul sous-pas reste temporisé, et
il est critique : la **remontée des doigts avant tout mouvement de chariot**
(§16 : ne jamais traîner un doigt sur la corde). `releaseComplete()` garde cette
fenêtre.

Conséquence de sûreté ajoutée : si le park gouverné **refuse** une commande de
repos, le homing est **abandonné** avec un hard-stop. Un doigt peut-être encore
posé ne doit jamais voir un chariot démarrer — c'est un cas qui n'existe pas
dans le build servo, où rien ne se déplace le long de la corde.

### P0.3 — un stepper arrêté est un stepper alimenté

`hardStop()` côté servo coupe `/OE` et les PWM directes. Côté moteur, arrêter
les impulsions STEP arrête le *mouvement*, mais le driver continue de tenir la
position en brûlant du courant : le moteur chauffe et le chariot reste bloqué.
`StepperBank::hardStop()` fait donc `stopAll()` **puis** coupe `ENABLE`, dans
cet ordre (jamais de driver dé-alimenté pendant que le moteur génère encore des
pas). Le park contrôlé est distinct : `controlledStopAll()` décélère et **garde**
les drivers alimentés, le temps de `stopDurationMs()`.

C'est aussi ce qui impose, côté matériel, un **deuxième contacteur (K2)** et une
grille sur `ENABLE` dans la chaîne d'arrêt d'urgence — documenté dans
[`hardware/POWER_AND_SAFETY.md`](hardware/POWER_AND_SAFETY.md) §3.1 et feuille 04.

### P1.6 — deux sources d'appel de courant, pas une

Le governor servo borne le démarrage simultané des servos. Ici s'ajoute
l'accélération simultanée des chariots d'un accord, sur un rail séparé. Le
**repositionnement de chariot est classé `Staggerable`** : il a du mou (la note
ne sonne qu'à `executeAtMs`), donc un permis différé ne coûte que du budget de
latence. La **presse du doigt à l'arrivée** et toutes les frappes restent
`Deadline` : jamais throttlées, parce qu'elles sont sur le chemin du son.

Les steppers n'étant pas sur un PCA9685, ils utilisent le bucket `0xFF` : seule
la limite globale s'applique, jamais une fenêtre par carte.

### P1.13 — le homing voyage avec l'**instrument**

Le split servo range `strings`/`servos` côté instrument et le reste côté device.
Ici il faut trancher sur `homing`, qui n'existe pas là-bas. Il décrit les
**chariots** (sens et vitesses de recherche, back-off, offset de repos, polarité
de chaque capteur), pas la carte : porter un instrument sur un autre ESP32 doit
emmener ces références, sinon le premier re-homing chercherait contre la
mauvaise. `homing` est donc côté **instrument**, et
`test_device_instrument.cpp` verrouille ce choix en déplaçant un instrument sur
un autre device.

---

## 4. Ce qui a été ajouté au-delà du dépôt servo

* **`stepperbankcheck`** — la branche Arduino de `StepperBank` n'était que
  *compilée*. Le harnais la fait tourner contre un stub FastAccelStepper
  instrumenté (qui peut manquer de générateurs de pas à la demande) et prouve :
  `hardStop` force-stoppe et coupe ENABLE alors que `controlledStopAll` décélère
  et le garde ; `moveToMm` rend `Ok`/`Disabled`/`OutputFault` ; le clamp aux
  butées logicielles ; un axe sans générateur est fauté **par axe** au lieu de
  ne simplement pas bouger.
* **`boardcheck`** — les JSON de `board-profiles/` sont désormais **générés**
  depuis `BoardProfile.cpp` (`firmware/tools/dump_board_profiles.cpp`) et la CI
  vérifie l'égalité. Deux copies manuelles d'une même table divergent toujours,
  et une copie divergée est exactement ce qui fait proposer par l'assistant une
  broche que le firmware refuse ensuite.
* **`SignalKind::SafetyInput`** — la broche `ESTOP` exige une entrée
  interruptible **avec pull-up interne** et **jamais une broche de strapping**
  (la boucle NC recommandée maintient la broche LOW pendant le boot, ce qui
  corromprait le strap).
* **Compteurs mouvement** dans `Diagnostics` : `axisMoves`, `homingFailures`,
  `limitTrips`, `moveTimeouts` — de quoi distinguer au banc un problème
  mécanique (un axe qui rate HOME) d'un problème électrique (un PCA qui tombe).
* **Page Instrument** repensée : le build servo montre *quel doigt presser* ;
  ici l'information utile est **où est chaque chariot**, en direct, avec un
  marqueur fantôme sur la cible commandée pendant le déplacement, et un jog par
  axe pour la mise au point et la calibration des frettes.

---

## 5. Écarts assumés

> Cette section listait quatre écarts. Trois ont été fermés depuis et la raison
> qui les justifiait mérite d'être conservée, parce qu'elle était bonne au moment
> où elle a été écrite et que c'est ce qui a changé, pas le raisonnement.

* **P2.17 (`main.cpp`)** — *fermé*. L'écart tenait à un argument juste : dans le
  dépôt servo les extractions sont *verbatim*, donc sûres par construction, alors
  qu'ici la FSM par corde est différente (mouvement du chariot, deadlines de
  déplacement, faute d'axe) et la déplacer ressemblait à une réécriture. La sortie
  a été de rendre l'extraction verbatim quand même : chaque méthode ré-alias ses
  collaborateurs vers les anciens noms `g_*`, et l'équivalence a été **vérifiée
  par diff** contre la révision précédente (`tickString` identique sur ses 284
  lignes significatives). Puis `runtimecheck` a remplacé la garantie « par
  construction » par des tests qui échouent quand on retire une garde.
* **P1.7 / P1.11 / P1.13** — *fermés*. Le câblage manquant a été fait : DIN sur
  UART2 avec une broche `MIDI_RX` configurable, `POST /api/midi/source` pour la
  posture UDP, et une persistance Device/Instrument qui tient aussi au boot.
  USB-MIDI reste la seule réserve, et elle est explicite (env dédié, non validé
  sur matériel).
* **`test_scheduler.cpp`, `test_pluck.cpp`, `test_fretservo.cpp`,
  `test_geared.cpp`** du dépôt servo ne sont **pas** repris : ils testent le
  modèle servo-par-frette (doigts engrenés, `PluckPlan`, `PlaybackScheduler`)
  qui n'a pas d'équivalent ici. L'équivalent pas-à-pas est
  `test/runtimecheck/main_playback.cpp`.
* **USB-MIDI non validé sur matériel** — le seul écart qui reste ouvert. Il
  compile et il est construit en CI ; personne ne l'a énuméré face à un hôte, et
  le tracker Adafruit TinyUSB porte des soucis spécifiques au S3. Voir
  [`docs/MIDI_PROTOCOL.md`](docs/MIDI_PROTOCOL.md) §1.2 pour ce que coûte
  l'activation (la console série du S3).

---

## 6. À valider physiquement

Rien de ce qui suit n'est prouvé par le logiciel :

* que `hardStop` coupe assez vite sous charge, et que le moteur devient
  réellement **libre** (pas de couple de maintien résiduel) ;
* que la remontée des doigts avant le seek couvre le temps mécanique réel ;
* le sens de rotation de chaque axe au premier seek (un `DIR` inversé part au
  bout de la course — c'est le défaut classique) ;
* que les mm affichés sont les mm parcourus (steps/mm : microsteps, poulie,
  pas de vis) — un jog **mesuré** est le seul test valable ;
* la répétabilité (pas perdus) sur 20 aller-retours vers la même frette ;
* l'appel de courant réel d'un accord sur le rail moteur, même avec le governor ;
* la détection I2C d'un PCA débranché *sous tension* ;
* la température des drivers après un passage soutenu — le courant de maintien
  coule pendant toute la durée où l'instrument est armé, pas seulement pendant
  les mouvements.

La procédure complète, étape par étape avec les mesures à consigner, est dans
[`hardware/COMMISSIONING.md`](hardware/COMMISSIONING.md) ; l'interface web la
reprend en checklist vivante (*Wiring & GPIO → Commissioning*).
