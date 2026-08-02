# Protocole MIDI — Stepper-Plucked-Strings-GMB

> Sources : `cahier des charges.md` §8 · `selection corde et frette.md` (intégral) · `Communication automatique des capacités par SysEx.md` (intégral).
> Code : `firmware/src/core/midi/{MidiEvent.h, StringFretSelector.*}`, `core/gmb/{GmbSysEx.*, Capabilities.*}`.
> Documents liés : [`ARCHITECTURE.md`](ARCHITECTURE.md) · [`WEB_INTERFACE.md`](WEB_INTERFACE.md).

Ce document couvre trois volets :

1. le transport MIDI Wi-Fi et l'événement interne `MidiEvent` (§8.2) ;
2. la sélection explicite corde/frette par CC (spec « selection corde et frette ») ;
3. le protocole de capacités SysEx GMB (spec « SysEx »).

---

## 1. Transport MIDI Wi-Fi et `MidiEvent` (§8.2)

La couche de transport est **séparée** du moteur MIDI interne. En première
version, les entrées Wi-Fi peuvent être :

* WebSocket binaire ;
* RTP-MIDI ;
* protocole UDP configurable ;
* commandes de test depuis l'interface Web.

Tous les transports produisent un **événement interne commun** :

```cpp
struct MidiEvent {
    uint32_t timestampUs = 0;
    uint8_t source;    // MidiSource (WifiWebSocket, WifiRtp, WifiUdp, WebUiTest, Ble, Usb, Din, Serial…)
    uint8_t type;      // MidiType : NoteOff 0x80, NoteOn 0x90, ControlChange 0xB0, SysEx 0xF0…
    uint8_t channel;   // 0..15 (interne, base 0)
    uint8_t data1;
    uint8_t data2;
    bool isNoteOn()  const;  // NoteOn avec vélocité > 0
    bool isNoteOff() const;  // NoteOff, OU NoteOn vélocité 0 (running status)
    bool isControlChange() const;
};
```

Extensions futures possibles sans modifier le cœur : BLE MIDI, USB MIDI, MIDI DIN,
liaison série, CAN/RS485. GPIO19/GPIO20 restent réservés pour l'USB natif.

---

## 2. Sélection explicite de la corde et de la frette par CC

Code : `core/midi/StringFretSelector.{h,cpp}`.

### 2.1 Objectif et convention

Le contrôleur peut recevoir une indication explicite de corde et de frette avant
un `Note On`, permettant à General-Midi-Boop de transmettre une position de
tablature :

```text
CC20 (corde) → CC21 (frette) → Note On
```

Convention par défaut : **CC20 = numéro de corde**, **CC21 = numéro de frette**.
Exemple : `CC20=3`, `CC21=5`, `Note On 60 vél 100` → jouer la note 60 sur la corde
physique 3, frette 5, vélocité 100.

Aucun numéro de CC ni aucune valeur n'est codé en dur : tout est modifiable depuis
l'interface Web.

### 2.2 Modes de sélection

| Mode | `SelectionMode` | Comportement |
| ---- | --------------- | ------------ |
| **Automatique** | `Automatic` (0) | ignore les CC, alloue toujours automatiquement ; compatible fichiers MIDI standards |
| **Explicite** | `Explicit` (1) | corde et frette imposées par les CC reçus avant le Note On |
| **Hybride** | `Hybrid` (2) | **par défaut** : CC si complets et valides et jouables, sinon repli sur l'allocation automatique |

Le mode hybride est le défaut car il lit à la fois des fichiers MIDI standards,
des fichiers enrichis, les commandes de General-Midi-Boop et les notes de l'UI.

### 2.3 Préréglage General-Midi-Boop (`applyGmbPreset()`)

| Paramètre | Valeur |
| --------- | -----: |
| Sélection explicite activée | oui |
| CC corde | 20 |
| CC frette | 21 |
| Première corde | valeur 1 |
| Première frette | valeur 0 |
| Offset corde / frette | 0 / 0 |
| Mode de consommation | prochaine note |
| Sélection par canal MIDI | oui |
| Allocation de secours | automatique |
| Préparation dès réception des CC | oui |

Les maxima sont adaptés au profil actif : CC corde 1…nombre de cordes, CC frette
0…frette maximale de l'instrument.

### 2.4 Réglages (`SelectorConfig`)

```cpp
struct SelectorConfig {
    bool enabled = true;
    SelectionMode mode = SelectionMode::Hybrid;
    bool perMidiChannel = true;
    uint32_t selectionTimeoutMs = 100;   // plage 5..2000 ms
    bool prepareOnCompleteSelection = true;
    uint16_t queueDepth = 32;            // >= 16
    StringSelectionConfig string;        // ccNumber=20, min=1, max=nbCordes, offset, numbering, reverseOrder, mapping[]
    FretSelectionConfig fret;            // ccNumber=21, min=0, max=frette, offset, invalidValuePolicy
    NotePositionPolicy notePositionPolicy = CcPriorityWithWarning;
    InvalidValuePolicy missingSelectionPolicy = AutomaticFallback;
    InvalidValuePolicy expiredSelectionPolicy = AutomaticFallback;
};
```

### 2.5 Transformation des valeurs

* **Corde** : `corde logique = valeur CC + offset`, puis validée, limitée à la
  plage, convertie vers l'indice interne (0-based) et associée à un axe. La
  numérotation (`ZeroBased`/`OneBased`), l'ordre (normal/inversé) et une table
  `mapping[]` personnalisée (indice logique → axe physique) sont appliqués.
  `mapStringValue(rawValue)` renvoie l'indice d'axe physique ou -1.
* **Frette** : `frette logique = valeur CC + offset`. `mapFretValue(rawValue)`
  renvoie la frette ou -1. **La frette 0 entraîne automatiquement : doigt relevé,
  aucun appui, pincement de la corde à vide.**

Exemple ordre normal vs inversé (4 cordes) :

| Valeur CC | Normal → corde physique | Inversé → corde physique |
| --------: | ----------------------: | -----------------------: |
| 1 | 1 | 4 |
| 2 | 2 | 3 |
| 3 | 3 | 2 |
| 4 | 4 | 1 |

### 2.6 Délai de validité (timeout)

Une sélection ne reste pas active indéfiniment : `selectionTimeoutMs` (défaut
100 ms, plage 5–2000 ms). Si aucun `Note On` correspondant n'arrive dans ce
délai, la sélection est supprimée (`expire(nowUs)` à appeler périodiquement).

### 2.7 Gestion fiable des accords — file FIFO

Le firmware **ne conserve pas** seulement « dernière corde / dernière frette
reçue » : cette méthode échouerait sur les accords. Il utilise une **file FIFO**
de sélections (`PendingStringSelection`), profondeur ≥ 16, recommandée 32.

```cpp
struct PendingStringSelection {
    uint8_t midiChannel;
    bool hasString, hasFret;
    uint8_t stringValue;   // indice d'axe physique (déjà mappé)
    uint8_t fretValue;
    uint32_t receivedAtUs, expiresAtUs;
    bool complete() const { return hasString && hasFret; }
};
```

Exemple : `CC20=1, CC20=3, CC20=4, CC21=2, CC21=5, CC21=7, NoteOn 42, 55, 64`
reconstruit : sélection1 = corde 1/frette 2, sélection2 = corde 3/frette 5,
sélection3 = corde 4/frette 7 ; puis Note 42 → sél1, Note 55 → sél2, Note 64 → sél3.

### 2.8 Algorithme d'association

* **CC de corde** (`onControlChange`) : lire, appliquer offset, convertir en corde
  physique, vérifier la plage, **créer** une nouvelle sélection en attente.
* **CC de frette** (`onControlChange`) : lire, offset, vérifier la plage,
  chercher la **plus ancienne sélection sans frette**, y ajouter la frette,
  marquer complète.
* **Note On** (`onNoteOn`) : chercher la **plus ancienne sélection complète du
  canal**, associer la note, valider la cohérence, retirer de la file, préparer le
  moteur, programmer appui et pincement. Renvoie :

```cpp
struct NoteResolution {
    bool play;
    ResolveSource source;   // Explicit / Automatic / Rejected
    uint8_t stringIndex, fret;
    uint32_t noteInstanceId;
    std::string warning;    // note non fatale (ex. incohérence note/frette)
};
```

### 2.9 Préparation anticipée

Si `prepareOnCompleteSelection` (défaut activé), dès qu'une paire corde/frette est
complète, le contrôleur peut commencer la préparation mécanique (relâchement du
doigt, déplacement du moteur) **sans attendre le Note On**. Le Note On conserve
son rôle de déclenchement musical. Si le moteur n'a pas atteint la frette au
moment du Note On : le pincement est mis en attente, le moteur termine, le doigt
appuie, puis le pincement s'exécute — **aucun pincement anticipé**.

### 2.10 Cohérence note / corde / frette (`NotePositionPolicy`)

Note attendue = `note à vide + numéro de frette + capo + transposition`.

| Politique | Comportement |
| --------- | ------------ |
| `CcPriorityWithWarning` (0) | **défaut** : corde/frette des CC utilisées, la différence avec le Note On est seulement signalée |
| `NotePriority` (1) | la frette est recalculée depuis la note MIDI |
| `Strict` (2) | la note est refusée si les informations sont incohérentes |

### 2.11 Gestion du Note Off

L'affectation réelle de chaque Note On est mémorisée. Le Note Off **n'utilise pas**
la dernière valeur de CC reçue : il retrouve l'affectation enregistrée
(`onNoteOff` renvoie l'`ActiveNote`).

```cpp
struct ActiveNote {
    uint8_t midiChannel, midiNote, stringIndex, fret;
    uint32_t noteInstanceId;
};
```

Cela relâche la bonne corde, gère plusieurs cordes simultanées et les accords, et
empêche qu'une nouvelle sélection modifie une note déjà active. Pour les notes
répétées de même hauteur, une pile d'instances est utilisée.

### 2.12 Valeurs invalides (`InvalidValuePolicy`)

| Politique | Comportement |
| --------- | ------------ |
| `Reject` (0) | refuser la commande |
| `Clamp` (1) | limiter à la plage autorisée |
| `AutomaticFallback` (2) | **défaut** : allocation automatique + avertissement |
| `LastValid` (3) | utiliser la dernière valeur valide |

Cas invalides : corde inexistante, frette hors capacité, axe désactivé, corde en
défaut, frette non calibrée, sélection expirée, paire CC incomplète.

### 2.13 Validation de la configuration (§18)

* CC corde et CC frette compris entre 0 et 119 (les **CC120–127** = messages de
  mode de canal, jamais proposés — cf. `kMaxAssignableCc = 119`) ;
* deux numéros de CC différents ;
* pas de conflit avec une autre fonction configurée ;
* plages corde/frette compatibles avec le profil ;
* profondeur de file suffisante (≥ 16) ;
* délai de validité non nul.

CC20 et CC21 sont présentés comme choix recommandés.

### 2.14 Profil JSON (§17)

```json
{
  "stringFretSelection": {
    "enabled": true,
    "mode": "hybrid",
    "preset": "general-midi-boop",
    "perMidiChannel": true,
    "selectionTimeoutMs": 100,
    "prepareOnCompleteSelection": true,
    "queueDepth": 32,
    "string": { "ccNumber": 20, "minimum": 1, "maximum": 6, "offset": 0,
                "numbering": "oneBased", "reverseOrder": false, "mapping": [0,1,2,3,4,5] },
    "fret":   { "ccNumber": 21, "minimum": 0, "maximum": 24, "offset": 0,
                "invalidValuePolicy": "automaticFallback" },
    "validation": { "notePositionPolicy": "ccPriorityWithWarning",
                    "missingSelectionPolicy": "automaticAllocation",
                    "expiredSelectionPolicy": "automaticAllocation" }
  }
}
```

Le moniteur MIDI Web (§15) et l'outil de test (§16) sont décrits dans
[`WEB_INTERFACE.md`](WEB_INTERFACE.md).

---

## 3. Protocole SysEx GMB

Code : `core/gmb/GmbSysEx.{h,cpp}` (encodeur/décodeur) et `core/gmb/Capabilities.{h,cpp}`
(construction du snapshot). Le service est **indépendant du transport** : il ne
manipule que des buffers d'octets MIDI complets.

### 3.1 En-tête

```text
F0 7D 00 <bloc> <direction> ... F7
```

| Octet | Fonction |
| ----- | -------- |
| `F0` | début SysEx (`kStart`) |
| `7D` | identifiant SysEx expérimental/éducatif (`kManufacturer`) |
| `00` | identifiant General-Midi-Boop (`kGmbId`) |
| `<bloc>` | type d'information (1/5/6/7/8) |
| `<direction>` | `00` requête, `01` réponse, `02` notification spontanée |
| `F7` | fin SysEx (`kEnd`) |

Taille maximale d'un message : `kMaxMessage = 512` octets.

### 3.2 Blocs implémentés

| Bloc | `SysExBlock` | Obligatoire | Rôle |
| ---- | ------------ | ----------- | ---- |
| 1 | `Identity` | oui | identité de l'appareil |
| 5 | `Descriptor` | recommandé | description de l'instrument |
| 6 | `Capabilities` | oui | capacités (plage jouable, polyphonie, CC…) |
| 7 | `StringConfig` | oui | configuration des cordes (v1 + v2) |
| 8 | `Notification` | extension | notification de modification (Capabilities Changed) |

### 3.3 Bloc 1 — Identité

Requête : `F0 7D 00 01 00 F7`. Réponse :

```text
F0 7D 00 01 01 <version> <device_id[5]> <device_name[32]> <firmware[3]> <features[5]> F7
```

* Nom tronqué/complété à 32 octets 7 bits.
* `firmware` = {major, minor, patch}.
* `features` (drapeaux) : `INSTRUMENT_CAPABILITIES 0x10 | STRING_CONFIG 0x20`
  → `0x30` ; avec le bloc 5, ajouter `INSTRUMENT_DESCRIPTOR 0x08` → `0x38`
  (valeur utilisée par `buildSnapshot`).
* Identifiant stable après redémarrage (ID matériel ESP32 et/ou valeur aléatoire
  sauvegardée). Un bouton avancé permet de le régénérer.

### 3.4 Bloc 5 — Descripteur (instrument unique)

Requête : `F0 7D 00 05 00 F7`. Réponse :

```text
F0 7D 00 05 01 01 01 <canal> <programme_gm> <type_id> F7
```

Exemple guitare nylon (GM 24 = `0x18`, type `0x04`) : `F0 7D 00 05 01 01 01 01 18 04 F7`.

### 3.5 Bloc 6 — Capacités

Requête : `F0 7D 00 06 00 <canal> F7`. Réponse (`encodeCapabilities`) :

```text
F0 7D 00 06 01
<version> <canal> <programme_gm> <type_id> <sous_type> <mode_notes>
<note_min> <note_max> <polyphonie>
<nombre_notes_discretes> <notes_discretes...>
<nombre_cc> <cc_supportes...>
<longueur_nom> <nom...>
F7
```

#### Plage jouable (§5) — calcul automatique

`buildSnapshot()` ne saisit pas la plage manuellement : pour chaque corde active,
`note min = note à vide + capo + transpose`, `note max = note min + frettes max de
la corde`. L'**union** de toutes les notes jouables est construite (bornée 0–127).

* **Mode plage continue** (`mode_notes = 0`) : si **toutes** les notes entre min
  et max sont jouables sur au moins une corde. Le bloc porte `note_min`,
  `note_max`, `nombre_notes_discretes = 0`.
* **Mode notes discrètes** (`mode_notes = 1`) : si certaines notes intermédiaires
  ne sont jouables sur aucune corde (accordages particuliers, peu de frettes,
  frettes inégales, cordes désactivées…). La liste exacte des notes jouables est
  transmise.

#### Polyphonie (§6)

`polyphonie = nombre de cordes actives et fonctionnelles` par défaut, ou valeur
personnalisée (`polyphonyOverride ≥ 0`). Exemples : 6 cordes à médiators
individuels → 6 ; 6 cordes avec contraintes limitant à 4 → configurée à 4.

#### Contrôleurs annoncés (§7)

Seuls les CC **réellement activés** sont annoncés, triés :

| CC | Fonction | Condition |
| -: | -------- | --------- |
| 7 | volume | toujours |
| 11 | expression | toujours |
| CC corde | sélection corde | si `selector.enabled` (numéro configuré, ex. 24) |
| CC frette | sélection frette | si `selector.enabled` (ex. 25) |
| 64 | maintien | si `midi.sustainPedal` |
| 120 | arrêt sonore immédiat | toujours |
| 123 | arrêt de toutes les notes | toujours |

Un CC désactivé n'est pas annoncé.

### 3.6 Bloc 7 — Configuration des cordes

Requête : `F0 7D 00 07 00 <canal> F7`.

#### Version 1 (compatible GMB) — `encodeStringConfigV1`

```text
F0 7D 00 07 01
01 <canal> <nombre_cordes> <nombre_frettes> <is_fretless> <capo>
<cc_active> <cc_corde> <cc_frette>
<accordage...>
F7
```

`is_fretless = 0` (toujours pour ce projet). Accordage transmis dans l'ordre
**grave → aigu** (ordre GMB). La v1 ne transmet pas : frettes par corde, min/max
des CC, offsets, ordre inversé, table personnalisée, mode de sélection.

#### Version 2 (extension) — `encodeStringConfigV2`

```text
F0 7D 00 07 01
02 <canal> <nombre_cordes> <nombre_frettes_global> <is_fretless> <capo>
<cc_active> <cc_corde> <cc_frette>
<cc_corde_min> <cc_corde_max> <cc_corde_offset_encode>
<cc_frette_min> <cc_frette_max> <cc_frette_offset_encode>
<mode_selection> <ordre_cordes>
<accordage[num]> <frettes_par_corde[num]> <mapping_cordes[num]>
F7
```

* **Mode de sélection** : 0 auto / 1 explicite / 2 hybride.
* **Ordre des cordes** : 0 normal / 1 inversé / 2 correspondance personnalisée.
* **Offsets signés** : encodés `valeur_transmise = offset + 64` (plage −64…+63),
  décodés `offset = valeur_transmise − 64` — pour rester en 7 bits.

Compatibilité : le firmware répond en **v1** tant que General-Midi-Boop ne
supporte pas la v2 (`respond(req, snap, useV2=false)` par défaut).

### 3.7 Bloc 8 — Notification de modification

`encodeNotification` :

```text
F0 7D 00 08 02
01 <canal> <revision_configuration[5]> <flags_modification>
F7
```

* La révision est encodée sur **5 octets 7 bits big-endian** (capacité 35 bits).
* Drapeaux (`ChangeFlag`) :

| Bit | `ChangeFlag` | Signification |
| --: | ------------ | ------------- |
| 0 | `kIdentityChanged` | identité modifiée |
| 1 | `kDescriptorChanged` | descripteur modifié |
| 2 | `kCapabilitiesChanged` | capacités générales modifiées |
| 3 | `kStringConfigChanged` | configuration des cordes modifiée |
| 4 | `kCcMappingChanged` | mapping CC modifié |
| 5 | `kRestartRequired` | redémarrage ou nouveau homing requis |
| 6 | réservé | — |

La notification ne transporte pas toutes les capacités : elle invite GMB à
relancer sa découverte (Bloc 1 → 6 → 7, et 5 si nécessaire). GMB ne remplace la
configuration que si toutes les réponses sont valides ; sinon l'ancienne reste
active avec un avertissement.

### 3.8 Numéro de révision (§13)

`capabilitiesRevision` (dans `Profile`) est incrémenté à chaque sauvegarde valide,
persistant, inclus dans la notification Bloc 8, affiché dans l'UI, et sert à
éviter les actualisations inutiles. Un simple redémarrage sans modification ne
l'incrémente pas nécessairement.

### 3.9 Snapshot immuable (§19)

```cpp
struct CapabilitySnapshot {
    uint32_t revision;
    DeviceIdentity identity;
    InstrumentDescriptor descriptor;
    InstrumentCapabilities capabilities;
    StringInstrumentConfig stringConfig;
    uint32_t generatedAtMs;
    bool valid;
};
CapabilitySnapshot buildSnapshot(const Profile& p, int polyphonyOverride = -1);
```

Une réponse SysEx utilise **un seul** snapshot : une modification pendant l'envoi
ne peut jamais mélanger deux versions du profil. Une configuration en brouillon
n'est **jamais** publiée — seul le profil validé et activé l'est.

### 3.10 Sécurité du protocole (§20)

`isWellFormed()` et `parseRequest()` appliquent :

* vérification de l'en-tête (`F0 7D 00`) et du trailer (`F7`) ;
* longueur minimale 6 octets, maximale 512 ;
* tous les octets internes limités à 7 bits (rejet si bit 0x80 posé) ;
* blocs 6 et 7 : octet de canal requis avant `F7` ;
* directions inconnues ignorées ; blocs inconnus ignorés (`respond` renvoie `{}`) ;
* aucune allocation dynamique pendant le traitement, réponses limitées en
  fréquence, canaux inexistants refusés ;
* **jamais** de mot de passe Wi-Fi ni de données sensibles transmis.

Les messages SysEx inconnus sont ignorés sans affecter le fonctionnement musical.

### 3.11 Indépendance du transport (§21)

```text
Wi-Fi MIDI ─┐
BLE MIDI ───┤
USB MIDI ───┼──► MidiMessageRouter ─► GmbSysExService
MIDI DIN ───┤
série ──────┘
```

Les futures versions Bluetooth/filaires réutilisent exactement les mêmes blocs,
encodeur, décodeur, snapshot et tests. La première version utilise le Wi-Fi.

### 3.12 Compatibilité initiale (§22)

Première version : Bloc 1 v1, Bloc 5 v1 (recommandé), Bloc 6 v1, Bloc 7 v1.
Extension coordonnée ultérieure : Bloc 7 v2 + Bloc 8. Tant que le Bloc 8 n'est pas
supporté côté GMB, l'interface fournit un bouton « Réannoncer les capacités ».
