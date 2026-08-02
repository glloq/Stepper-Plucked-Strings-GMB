# Interface Web — Stepper-Plucked-Strings-GMB

> Sources : `cahier des charges.md` §9, §10, §18, §19, §20 · `selection corde et frette.md` §14–16 · `Communication automatique des capacités par SysEx.md` §17–18.
> Documents liés : [`ARCHITECTURE.md`](ARCHITECTURE.md) · [`PIN_CONFIGURATION.md`](PIN_CONFIGURATION.md) · [`MIDI_PROTOCOL.md`](MIDI_PROTOCOL.md) · [`FIRST_CONFIGURATION.md`](FIRST_CONFIGURATION.md).

L'interface Web permet à un débutant de configurer l'instrument sans modifier le
code source, depuis un ordinateur, une tablette ou un téléphone. Aucune
application dédiée n'est nécessaire.

---

## 1. Deux niveaux d'interface (§9.2)

### Mode simplifié (débutant)

Assistant étape par étape, valeurs recommandées, attribution automatique des
broches, schémas de branchement, boutons de test, validation automatique, messages
d'erreur compréhensibles. Ne montre par défaut que les GPIO **verts** (voir
[`PIN_CONFIGURATION.md`](PIN_CONFIGURATION.md)).

### Mode avancé (mise au point)

Attribution manuelle des GPIO (y compris les broches **jaunes** avec explication),
réglage des vitesses/accélérations/délais, courbes de vélocité, diagnostics,
édition des paramètres détaillés, import/export JSON.

---

## 2. Assistant de première configuration — 9 étapes (§10)

| Étape | Titre | Contenu |
| ----- | ----- | ------- |
| 1 | **Identification** | nom, description, nombre de cordes, type d'instrument, accordage proposé, nombre max de frettes |
| 2 | **Choix de la carte** | modèle ESP32, révision, Flash, PSRAM, variante → détermine GPIO disponibles/réservés/recommandés (profil `esp32-s3-devkitc-1`) |
| 3 | **Attribution automatique** | bouton « Attribuer automatiquement les broches » (nb cordes, interfaces, carte, USB futur, port diagnostic, I²C, capteurs) |
| 4 | **Configuration mécanique** | par corde : note à vide, frettes max, longueur vibrante, transmission, pas/tour, microstepping, déplacement/tour, inversion, vitesse/accélération max, position de repos |
| 5 | **Homing** | par axe : capteur activé, GPIO, NO/NC, niveau actif, direction, vitesses rapide/lente, recul, offset, timeout, distance max |
| 6 | **Calibration des servos** | par servo : canal PCA9685, repos, actif, limites, inversion, temps de déplacement/stabilisation, désactivation au repos |
| 7 | **Calibration des notes** | calcul automatique des frettes **ou** calibration manuelle de chaque position |
| 8 | **Test** | tester chaque moteur, capteur, doigt, médiator, note, corde, un accord, l'arrêt général |
| 9 | **Validation** | « Configuration valide » ou liste précise des problèmes ; aucun actionneur activé tant que les erreurs critiques ne sont pas corrigées |

Le détail pas à pas est dans [`FIRST_CONFIGURATION.md`](FIRST_CONFIGURATION.md).
Les calculs des étapes 4–7 sont dans [`CALIBRATION.md`](CALIBRATION.md).

---

## 3. Pages de l'interface

### 3.1 Tableau de bord (§19)

Page principale — état général :

```text
état général · connexion Wi-Fi · source MIDI · profil actif ·
nombre de cordes prêtes · notes jouées · défauts actifs ·
températures · tensions · bouton STOP
```

Par corde : état (machine d'état), note actuelle, fret actuel, position moteur,
position cible, distance restante, état HOME, état LIMIT, état du doigt, état du
médiator, dernier défaut.

### 3.2 Page MIDI — sélection corde/frette (selection corde et frette §14–16)

Écran **simplifié** (§14) :

```text
[✓] Activer la sélection corde/frette
Système utilisé : [ General-Midi-Boop ]
CC de corde : [ 20 ]      CC de frette : [ 21 ]
Numérotation des cordes : [ 1 à 6 ]
Ordre des cordes : [ Normal ]
En cas de CC absent : [ Choisir automatiquement ]
```

Boutons : Appliquer le préréglage · Tester la réception · Envoyer un test · Voir
les valeurs reçues. Les réglages avancés (offsets, tables, politiques) restent
masqués dans « Réglages avancés » (voir [`MIDI_PROTOCOL.md`](MIDI_PROTOCOL.md) §2).

**Moniteur MIDI Web (§15)** — temps réel :

| Temps | Canal | Message | Valeur | Interprétation |
| ----: | ----: | ------- | -----: | -------------- |
| 0 ms | 1 | CC20 | 3 | corde 3 |
| 1 ms | 1 | CC21 | 5 | frette 5 |
| 2 ms | 1 | Note On 60 | 100 | corde 3, frette 5 |

Affiche aussi : sélection complète / en attente / expirée, valeur invalide,
allocation automatique utilisée, incohérence note/frette, corde physique réelle.
Bouton pour vider le journal.

**Outil de test intégré (§16)** — choisir corde, frette, note MIDI, vélocité,
canal ; envoie automatiquement CC corde → CC frette → Note On → Note Off après une
durée choisie, et affiche chaque étape (CC reçu, sélection validée, axe en
déplacement, position atteinte, doigt appuyé, corde pincée).

### 3.3 Page MIDI — Identité et capacités GMB (SysEx §17–18)

Chemin : `MIDI > Identité et capacités GMB`.

**Mode simplifié (§17.1)** : activation de la détection GMB, nom, type, préréglage
d'instrument, programme GM, canal MIDI, boutons « Publier les capacités » et
« Tester la communication », état de la dernière détection. Capacités calculées en
lecture seule :

```text
Cordes : 4 · Frettes : 12 · Plage MIDI : 40 à 76 · Polyphonie : 4
CC corde : 20 · CC frette : 21 · Accordage : E2 A2 D3 G3 · Révision : 7
```

**Mode avancé (§17.2)** : activation des blocs 5/6/7, choix de la version du bloc 7,
surcharge de la polyphonie, plage continue ou notes discrètes, visualisation des
CC annoncés et des octets SysEx, envoi manuel de chaque réponse, envoi de la
notification, réinitialisation de l'identifiant, export du snapshot.

**Testeur SysEx (§18)** : simuler « Demander identité / descripteur / capacités /
configuration des cordes / Notifier une modification / Découverte complète ». Pour
chaque test : message envoyé, message reçu, décodage des champs, validité 7 bits,
longueur, erreur éventuelle, durée de réponse. Détails du protocole dans
[`MIDI_PROTOCOL.md`](MIDI_PROTOCOL.md) §3.

### 3.4 Paramètres MIDI (§18)

Canal global, mode Omni, canal par corde, transposition générale/par corde, plage
de notes, courbe de vélocité (linéaire / douce / forte / exponentielle /
personnalisée), comportement Note Off, pédale de maintien, délai de regroupement
des accords (défaut 3 ms), stratégie de saturation (voir `NoteAllocator`,
[`ARCHITECTURE.md`](ARCHITECTURE.md)). La vélocité peut agir sur la course/vitesse
du médiator, le délai d'attaque, le profil de pincement.

### 3.5 Profils (§20)

Au moins **8 profils**. Fonctions : créer, copier, renommer, supprimer, exporter,
importer, restaurer, définir le profil de démarrage. Format d'échange **JSON** :

```json
{
  "project": "Stepper-Plucked-Strings-GMB",
  "profileVersion": 1,
  "instrument": { "name": "Ukulele 4 cordes", "stringCount": 4, "pluckMode": "individual" },
  "board": { "profile": "esp32-s3-devkitc-1", "reserveUsb": true, "automaticPinAssignment": true },
  "network": { "mode": "station", "hostname": "gmb-ukulele" },
  "strings": []
}
```

Le mot de passe Wi-Fi n'apparaît **jamais** dans les exports ordinaires (sauf
option explicite).

---

## 4. API REST / WebSocket (adaptateur `web/`)

> API supposée pour la couche Web (module §23 `web/RestApi`, `web/WebSocketStatus`,
> `communication/WebSocketMidi`). Elle expose le cœur `Profile` / `PinManager` /
> `SafetyManager` / `GmbSysEx` décrits dans [`ARCHITECTURE.md`](ARCHITECTURE.md).

### 4.1 Endpoints REST

| Méthode | Endpoint | Rôle |
| ------- | -------- | ---- |
| `GET` | `/api/status` | état global + par corde (tableau de bord §19) |
| `GET` | `/api/profile` | profil actif (JSON) |
| `PUT` | `/api/profile` | remplacer le profil (brouillon → validation → activation) |
| `GET` | `/api/profiles` | liste des profils sauvegardés |
| `POST` | `/api/profiles` | créer / copier / importer un profil |
| `GET` | `/api/board/{id}` | profil de carte + capacités des GPIO (couleurs, filtrage) |
| `POST` | `/api/pins/auto` | attribution automatique (`PinRequest`) → assignations |
| `POST` | `/api/pins/validate` | validation des broches → liste de `PinError` |
| `POST` | `/api/panic` | panic logiciel (`SafetyManager::panic`) |
| `POST` | `/api/test/note` | jouer une note de test (corde, frette, note, vélocité, canal) |
| `POST` | `/api/sysex/request` | simuler une requête SysEx GMB → réponse décodée |
| `GET` | `/api/capabilities` | snapshot de capacités courant (lecture seule) |

### 4.2 WebSocket

| Canal | Rôle |
| ----- | ---- |
| `WS /ws/midi` | flux MIDI entrant/sortant (moniteur MIDI §15, transport WebSocket binaire) |
| `WS /ws/status` | statut temps réel du tableau de bord et par corde (§19) |

Notes :

* `PUT /api/profile` suit le flux brouillon → `ProfileValidator` → sauvegarde
  atomique → incrément `capabilitiesRevision` → reconstruction du snapshot →
  notification Bloc 8 (voir [`MIDI_PROTOCOL.md`](MIDI_PROTOCOL.md) §3.7). Une
  configuration en brouillon n'est **jamais** publiée.
* `POST /api/pins/auto` et `/api/pins/validate` correspondent directement à
  `PinManager::autoAssign` / `validate` (voir [`PIN_CONFIGURATION.md`](PIN_CONFIGURATION.md)).
* `POST /api/panic` et l'état de sécurité : voir [`SAFETY.md`](SAFETY.md).
