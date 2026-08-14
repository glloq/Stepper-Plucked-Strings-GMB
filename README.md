# Stepper-Plucked-Strings-GMB

**Transformez un vrai instrument à cordes en robot piloté en MIDI.**

Envoyez-lui des notes MIDI par Wi-Fi et il joue du ukulélé, de la guitare, de la
basse, de la mandoline ou du banjo — un moteur pas-à-pas fait glisser un « doigt »
le long de chaque corde pour choisir la note, et de petits servos pressent la
corde puis la pincent. Tout se configure depuis une **page web dans le
navigateur** ; aucune application à installer.

> Conçu pour l'**ESP32-S3** (et les cartes ESP32 classiques). Le cerveau est un
> cœur C++ portable et testé unitairement ; la partie ESP32 n'est que la colle
> matérielle.

[![CI](https://github.com/glloq/Stepper-Plucked-Strings-GMB/actions/workflows/ci.yml/badge.svg)](https://github.com/glloq/Stepper-Plucked-Strings-GMB/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/ESP32-S3%20%7C%20WROOM--32%20%7C%20DevKit%20v1-informational.svg)](https://www.espressif.com/en/products/socs/esp32)
[![Build](https://img.shields.io/badge/build-PlatformIO%20%7C%20Arduino-orange.svg)](https://platformio.org/)
[![MIDI](https://img.shields.io/badge/MIDI-Wi--Fi%20(UDP)-green.svg)](https://www.midi.org/)

*(English version: [`README_EN.md`](README_EN.md).)*

---

## Comment ça marche

Une **corde** = un **moteur** + quelques **servos** :

```
        ┌──────────────────── une corde ─────────────────────┐

   sillet                     doigt mobile                  chevalet
    │                            ▼                             │
    ╞════════════════════════════●═════════════════════════════╡  ← la corde
    0    1    2    3    4    5   position des frettes (mm)
    │                            ▲                             │
    │                 le moteur pas-à-pas amène                │
    │                 le doigt sur la frette                   │
    │                                                          │
    └─ servos :  [doigt] presse   [pluck] pince la corde       ┘
                 [étouffoir] mute  (chaque corde a son propre pinceur)
```

Pour jouer une note, le firmware :

1. **déplace** le chariot pour amener le doigt sur la bonne frette,
2. **presse** le doigt avec un servo,
3. **pince** la corde,
4. **l'étouffe** à la fin de la note.

Jusqu'à **6 cordes** fonctionnent indépendamment et en parallèle : les accords
sont donc possibles.

### Le chemin du signal

```
MIDI par Wi-Fi ─▶ parsing ─▶ choix corde & frette ─▶ allocation des notes
                                                        │
                                                        ▼
                                machine à états par corde (déplacer → presser → pincer)
                                                        │
                                                        ▼
                                    moteurs pas-à-pas  +  servos (PCA9685 ou GPIO)
```

Un contrôleur comme **General-Midi-Boop** peut aussi demander à l'instrument, en
MIDI SysEx, *« combien de cordes as-tu, quelle est ton étendue, quels CC
comprends-tu ? »* et s'adapter automatiquement.

---

## À quoi ça ressemble

Tout se configure et se joue depuis une page web servie par l'ESP32 — trois pages
et une fenêtre de réglages, sans rien à installer.

![La page Instrument](img/screenshots/instrument.png)

*La page **Instrument** : une voie par corde, chaque chariot dessiné là où il est
réellement. Cliquez une frette pour la jouer — cela envoie une vraie note MIDI
dans toute la chaîne, donc cela teste ce qu'un contrôleur obtiendrait.*

| | |
| --- | --- |
| [![Assistant de configuration](img/screenshots/setup-mechanics.png)](docs/WEB_INTERFACE.md#3-setup--nine-steps) | [![Faisceau de câblage](img/screenshots/wiring.png)](docs/WEB_INTERFACE.md#41-harness) |
| **Setup** — neuf étapes, de l'identité à la validation | **Wiring & GPIO** — le faisceau de *votre* configuration |
| [![Alimentation et sécurité](img/screenshots/wiring-power.png)](docs/WEB_INTERFACE.md#42-power--safety) | [![Diagnostics](img/screenshots/settings-diagnostics.png)](docs/WEB_INTERFACE.md#52-diagnostics) |
| **Power & safety** — le circuit de référence, dimensionné pour vos courants | **Diagnostics** — gigue de boucle, MIDI perdu, compteurs de mouvement |

La visite complète, page par page, est dans
[`docs/WEB_INTERFACE.md`](docs/WEB_INTERFACE.md). Toutes les captures sont
générées depuis l'interface réelle tournant sur son backend simulé — sans
matériel : `node web-interface/tools/screenshots.js`.

---

## Fonctionnalités

- 🎸 **1 à 6 cordes**, chacune avec son moteur, son doigt, son pinceur et un
  étouffoir optionnel.
- 🎵 **Allocation automatique des notes** — envoyez de simples notes MIDI et il
  répartit les accords sur les cordes, ou **forcez une corde/frette exacte** en
  MIDI CC (tablature).
- 🤙 **Pincement par corde** — chaque corde a son propre percuteur : un plectre ou
  un servo de grattage dédié.
- 🛰️ **MIDI Wi-Fi** — joue les notes reçues sur le réseau (UDP, port 5006).
- 🖥️ **Interface web locale** — assistant de configuration, tableau de bord en
  direct, moniteur MIDI, testeur SysEx. Tourne entièrement sur l'ESP32, sans
  cloud, et **compilée dans le firmware** : rien à téléverser séparément.
- 🧩 **Annonce des capacités (SysEx)** — protocole **GMB v2** : handshake puis
  descripteur JSON, également servi en HTTP sur `/gmb/descriptor.json`.
- 🛡️ **La sécurité d'abord** — homing avant tout jeu, démarrage **boot-safe** (sans
  profil valide il se verrouille en CONFIG_SAFE au lieu d'en inventer un), un vrai
  chemin d'arrêt dur distinct du rangement contrôlé, isolation des défauts par
  axe, surveillance des fins de course et câblage d'arrêt d'urgence fail-safe.
- ⚡ **Limiteur d'appel de courant** — dans un accord, les démarrages gourmands
  (repositionnement des chariots, appuis des doigts) sont étalés, tandis que les
  frappes qui portent le son ne sont jamais retardées.
- 📈 **Diagnostics d'exécution** — `GET /api/diagnostics` : latence et gigue de
  boucle, MIDI perdu, échecs de homing, déclenchements LIMIT, dépassements de
  temps de mouvement, santé des cartes PCA.
- 🔧 **Servos à votre façon** — cartes PCA9685 sur **l'un ou l'autre** des deux bus
  I²C de l'ESP32-S3 *ou* broches ESP32 directes, mélangeables servo par servo.
- 🧰 **Trois cartes supportées** — ESP32-S3-DevKitC-1 (les deux révisions),
  ESP32-WROOM-32 et ESP32 DevKit v1 ; chacune est compilée en CI, et c'est ce que
  « supportée » veut dire ici.

### Les instruments qu'il connaît déjà

Des profils prêts à l'emploi sont dans
[`instrument-profiles/`](instrument-profiles/) : **ukulélé**, **guitare**,
**basse**, **mandoline**, **banjo**. Chacun est un fichier JSON modifiable ou
copiable depuis l'assistant web.

---

## Démarrage rapide

### 1. Essayer la logique sur votre PC (sans matériel)

Tout le cerveau musical est du C++ pur et tourne sur votre portable :

```bash
cd firmware/test
make            # compile et exécute la suite de tests unitaires
```

Vous devez voir `… tests, … checks, 0 failures` (le badge CI fait foi pour le
compte exact).

### 2. Compiler et flasher le firmware

Vous pouvez utiliser **PlatformIO** ou l'**IDE Arduino** — mêmes sources.

**PlatformIO**

```bash
cd firmware
pio run                     # compile pour l'ESP32-S3-DevKitC-1 (par défaut)
pio run -e esp32-wroom-32   # …ou une carte ESP32 classique
pio run -t upload           # flashe le firmware — l'interface web est dedans
```

L'interface web est **embarquée dans l'image du firmware** : il n'y a pas de
téléversement de système de fichiers à oublier. Téléverser LittleFS
(`./sync_web_data.sh` puis `pio run -t uploadfs`) reste possible comme
surcharge fichier par fichier, pour modifier l'interface sur un appareil sans
recompiler.

**IDE Arduino** — ouvrez `firmware/firmware.ino` (le dossier `src/` est compilé
récursivement). Guide complet : [`docs/ARDUINO_IDE.md`](docs/ARDUINO_IDE.md).

### 3. Première configuration

Au premier démarrage, l'ESP32 crée un point d'accès Wi-Fi nommé
**`Stepper-Plucked-Strings-GMB`**. Connectez-vous dessus, ouvrez l'adresse de
l'appareil dans un navigateur, et l'**assistant de configuration** vous guide
pour les broches, les cordes et les servos. Voir
[`docs/FIRST_CONFIGURATION.md`](docs/FIRST_CONFIGURATION.md).

---

## Organisation du dépôt

```text
Stepper-Plucked-Strings-GMB/
├── firmware/            Firmware ESP32-S3
│   ├── src/core/        Logique C++ portable (MIDI, allocation, mouvement, sécurité) — testée
│   ├── src/platform/    Adaptateurs ESP32 (Wi-Fi, serveur web, drivers, stockage)
│   ├── src/main.cpp     Intégration matérielle / point d'entrée
│   └── test/            Suite de tests native (avec g++)
├── web-interface/       Application web locale (assistant, tableau de bord, moniteur MIDI, testeur SysEx)
├── instrument-profiles/ Instruments d'exemple (ukulélé, guitare, basse, mandoline, banjo)
├── board-profiles/      Cartographies de broches — GÉNÉRÉES depuis BoardProfile.cpp
├── hardware/            Électronique de référence, schémas, mise en service, BOM
├── mechanics/           Conception mécanique par corde
└── docs/                Guides et référence (voir ci-dessous)
```

**La conception logicielle en une ligne :** un cœur C++17 pur
(`firmware/src/core/`, sans dépendance Arduino, testé sur PC) plus de fins
adaptateurs ESP32 (`firmware/src/platform/esp32/`). Détails dans
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

---

## Documentation

| Guide | Contenu |
| ----- | ------- |
| [Architecture](docs/ARCHITECTURE.md) | Comment le code est structuré |
| [Première configuration](docs/FIRST_CONFIGURATION.md) | Parcours de l'assistant |
| [Interface web](docs/WEB_INTERFACE.md) | Chaque page de l'interface locale |
| [Protocole MIDI](docs/MIDI_PROTOCOL.md) | Notes, sélection corde/frette en CC, SysEx |
| [Configuration des broches](docs/PIN_CONFIGURATION.md) | Affectation et validation des GPIO |
| [Calibration](docs/CALIBRATION.md) | Positions de frettes, homing, mécanique |
| [Sécurité](docs/SAFETY.md) | Boot-safe, homing, arrêt dur vs rangement, arrêt d'urgence, défauts |
| [Circuit d'alimentation et sécurité](hardware/POWER_AND_SAFETY.md) | L'architecture électrique de référence : trois rails, chaîne d'arrêt d'urgence, `/OE` et `ENABLE` des drivers |
| [Mise en service](hardware/COMMISSIONING.md) | Procédure de recette par étapes de mise sous tension |
| [Schémas](hardware/schematics/README.md) | Distribution d'alimentation, arrêt d'urgence, branche PCA, driver pas-à-pas |
| [Réseau et hotspot](docs/NETWORK_HOTSPOT.md) | Station/AP, le hotspot au bouton BOOT, portail captif |
| [Appareil vs instrument](docs/DEVICE_INSTRUMENT.md) | Ce qui suit la machine et ce qui suit l'instrument |
| [Généralisation](docs/GENERALIZATION.md) | Où vivent les hypothèses 6 cordes / 24 frettes |
| [IDE Arduino](docs/ARDUINO_IDE.md) | Compiler sans PlatformIO |

Les spécifications d'origine sont les trois fichiers markdown à la racine : les
exigences complètes ([`SPECIFICATION.md`](SPECIFICATION.md)), la spécification de
sélection corde/frette en CC
([`STRING_FRET_SELECTION.md`](STRING_FRET_SELECTION.md)), et le protocole SysEx
d'annonce des capacités
([`SYSEX_CAPABILITIES.md`](SYSEX_CAPABILITIES.md)).

---

## État du projet

**Ce qui est fait et vérifié en CI :**

- Cœur logique complet et testé unitairement (tests natifs sous `-Werror`, puis à
  nouveau sous AddressSanitizer + UBSan).
- Les trois compilations de cartes ESP32 (PlatformIO) plus une vérification de
  compilation hôte rapide de `main.cpp` et de chaque adaptateur de plateforme.
- Des bancs d'essai d'exécution pour le code sous Arduino que les tests unitaires
  n'atteignent pas : `servobankcheck` (routage deux bus, rangement contrôlé,
  `ActuatorResult`) et `stepperbankcheck` (arrêt dur vs arrêt contrôlé, mouvements
  refusés, limites logicielles, générateur de pas manquant).
- Chaque profil d'instrument livré chargé par le vrai parseur du firmware, plus la
  migration v1→v2 et l'aller-retour du stockage en deux moitiés.
- Les profils de cartes JSON sont vérifiés comme correspondant toujours à
  `BoardProfile.cpp` — la table de broches de l'assistant et celle du validateur
  ne peuvent pas diverger.
- L'interface web (JS vanilla, sans étape de build) est vérifiée
  syntaxiquement, testée sur sa logique pure, et **montée dans un vrai navigateur**
  (chaque vue, chaque étape, chaque onglet).

**Pas encore fait — la validation matérielle.** Le firmware n'a **pas** été
exécuté sur un instrument physique. Le timing des pas à l'analyseur logique, six
axes simultanés, l'endurance MIDI, le comportement avec des capteurs
défectueux/absents/inversés, l'appel de courant réel d'un accord même avec le
limiteur, et la question de savoir si un arrêt dur coupe assez vite en charge :
tout cela demande encore un vrai banc. Tout ce qui précède est vérifié *en
logiciel*. Commencez par
[`hardware/COMMISSIONING.md`](hardware/COMMISSIONING.md). Considérez l'état actuel
comme **prêt pour une mise en route au banc**, pas pour un instrument entièrement
cordé, sous tension et sans surveillance.

Les limitations connues et la feuille de route sont listées en bas de
[`docs/SAFETY.md`](docs/SAFETY.md) et au fil de la documentation.

### Note de sécurité

L'arrêt d'urgence logiciel est un confort, **pas** un substitut à une coupure
matérielle de l'`ENABLE` des drivers et de l'alimentation moteur. Un moteur
pas-à-pas tient sa position en brûlant du courant dans ses bobines : arrêter les
impulsions STEP arrête le *mouvement*, mais le driver reste alimenté, le moteur
reste chaud et le chariot reste bloqué jusqu'à ce qu'`ENABLE` devienne inactif ou
que le rail disparaisse. Câblez un arrêt d'urgence physique avant de mettre les
moteurs en charge — circuit de référence dans
[`hardware/POWER_AND_SAFETY.md`](hardware/POWER_AND_SAFETY.md), comportement du
firmware dans [`docs/SAFETY.md`](docs/SAFETY.md).
