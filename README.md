# Stepper-Plucked-Strings-GMB

Contrôleur MIDI modulaire pour instruments à cordes **pincées ou grattées**
(ukulélé, guitare, basse, mandoline, banjo…). Un moteur pas à pas déplace un
**doigt mobile unique par corde** pour sélectionner la note ; des servomoteurs
assurent l'appui du doigt et le pincement. Plateforme cible **ESP32-S3**,
configuration par **interface Web locale**, communication **Wi-Fi MIDI**.

Ce dépôt implémente ce qui est décrit dans les spécifications à la racine :

| Spécification | Fichier |
| ------------- | ------- |
| Cahier des charges complet | [`cahier des charges.md`](cahier%20des%20charges.md) |
| Sélection explicite corde/frette par MIDI CC | [`selection corde et frette.md`](selection%20corde%20et%20frette.md) |
| Communication des capacités par SysEx (General-Midi-Boop) | [`Communication automatique des capacités par SysEx.md`](Communication%20automatique%20des%20capacit%C3%A9s%20par%20SysEx.md) |

## Organisation du dépôt

```text
Stepper-Plucked-Strings-GMB/
├── firmware/            Firmware ESP32-S3 (cœur C++ pur + adaptateurs + tests natifs)
│   ├── src/core/        Logique indépendante de la plateforme (testée sur hôte)
│   ├── src/platform/    Adaptateurs ESP32 (Wi-Fi, Web, PCA9685, drivers, LittleFS)
│   ├── src/main.cpp     Point d'entrée / intégration matérielle
│   ├── test/            Suite de tests unitaires natifs (g++)
│   └── platformio.ini   Build ESP32 (PlatformIO)
├── web-interface/       Interface Web locale (assistant, moniteur MIDI, testeur SysEx)
├── board-profiles/      Profils de cartes (ESP32-S3-DevKitC-1)
├── instrument-profiles/ Profils d'exemple (ukulélé, guitare, basse, mandoline, banjo)
├── hardware/            Électronique de référence, câblage, nomenclature
├── mechanics/           Architecture mécanique par corde
├── docs/                Architecture, GPIO, MIDI, calibration, sécurité, guides
└── tests/               (voir firmware/test)
```

## Architecture logicielle

Le firmware est séparé en un **cœur C++17 pur** (`firmware/src/core/`) sans
dépendance Arduino, et des **adaptateurs de plateforme** ESP32
(`firmware/src/platform/esp32/`). Le cœur est donc testable et vérifié sur un
PC de développement.

```text
transport (Wi-Fi UDP/WS) → MidiParser → MidiEvent
        → StringFretSelector (CC20/21, FIFO, hybride)
        → NoteAllocator (auto/accords/saturation)
        → StringController (machine d'état, id de commande annulable)
        → StepperBank (mm→pas) + ServoBank (PCA9685)

profil actif → CapabilitySnapshot → GmbSysExService (blocs 1/5/6/7/8)
```

Modules du cœur : `board/` (profils GPIO, attribution & validation),
`midi/` (événement, parseur, sélection corde/frette), `instrument/`
(allocation, machine d'état, contrôleur), `motion/` (géométrie pas/mm, homing
non bloquant), `configuration/` (profil + validateur), `gmb/` (capacités +
SysEx), `safety/` (sécurité & défauts).

Voir [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Compiler et tester le cœur (sur PC)

Le cœur se compile et se teste avec un simple `g++`, sans matériel :

```bash
cd firmware/test
make            # compile et exécute la suite de tests
```

Résultat attendu : `60 tests, … checks, 0 failures`. Les tests couvrent les
critères d'acceptation des trois spécifications (attribution/validation GPIO,
sélection corde/frette, accords, homing, machine d'état, SysEx…).

## Construire le firmware ESP32-S3

Le firmware se construit **au choix avec PlatformIO ou l'IDE Arduino** — même
code source.

**PlatformIO :**

```bash
cd firmware
./sync_web_data.sh          # copie web-interface/ vers data/www
pio run                     # build (env esp32-s3-devkitc-1)
pio run -t uploadfs         # téléverser l'interface Web vers LittleFS (/www)
pio run -t upload           # flasher le firmware
```

**IDE Arduino :** ouvrez `firmware/firmware.ino`. Le dossier `src/` est compilé
récursivement. Voir le guide complet (cartes, bibliothèques, LittleFS) :
[`docs/ARDUINO_IDE.md`](docs/ARDUINO_IDE.md).

Au premier démarrage, l'ESP32 crée le point d'accès
`Stepper-Plucked-Strings-GMB` ; connectez-vous et ouvrez son adresse locale
pour lancer l'assistant de configuration
(voir [`docs/FIRST_CONFIGURATION.md`](docs/FIRST_CONFIGURATION.md)).

## État d'avancement

Implémenté et testé : cœur logique complet (GPIO, MIDI, sélection corde/frette,
allocation, homing, machines d'état, profils, capacités/SysEx, sécurité),
adaptateurs ESP32, interface Web, profils d'exemple, documentation. Le matériel
dédié (schéma, PCB — phase 5 du cahier des charges) reste au stade de
documentation.
