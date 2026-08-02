# Sécurité — Stepper-Plucked-Strings-GMB

> Sources : `cahier des charges.md` §21, §22 · Code : `core/safety/SafetyManager.{h,cpp}`.
> Documents liés : [`ARCHITECTURE.md`](ARCHITECTURE.md) · [`CALIBRATION.md`](CALIBRATION.md) · [`WEB_INTERFACE.md`](WEB_INTERFACE.md).

Le `SafetyManager` centralise les états sûrs, le panic logiciel, l'arrêt d'urgence
matériel et le journal des défauts.

```cpp
enum class SafetyState {
    PowerOnSafe,   // drivers off, servos neutralisés, files vides
    Armed,         // fonctionnement normal
    Panic,         // panic logiciel verrouillé
    EmergencyStop, // arrêt matériel asserté
};
```

`actuatorsAllowed()` ne renvoie `true` que dans l'état `Armed` : aucun actionneur
ne bouge dans les autres états.

---

## 1. État au démarrage (§21.1)

À la mise sous tension, `boot()` place le système en `PowerOnSafe` :

```text
drivers désactivés
servos neutralisés
sorties auxiliaires coupées
files MIDI vides
profil vérifié
GPIO validés
```

La transition vers `Armed` n'est possible **qu'après** validation du profil et des
GPIO :

```cpp
bool arm(bool profileValid, bool pinsValid);  // Armed uniquement si les deux sont vrais
```

Aucun actionneur n'est activé en mode normal tant que les erreurs critiques ne
sont pas corrigées (cf. assistant §9, [`WEB_INTERFACE.md`](WEB_INTERFACE.md)).

### Séquence de démarrage complète (§13 / §21.1)

`main.cpp` suit une machine à trois phases ; **le jeu n'est armé qu'après un
homing réussi**, de sorte qu'aucun axe ne bouge depuis une position physique
inconnue :

```text
Boot     : PowerOnSafe — profil chargé et validé, drivers OFF, servos au repos
   │        (si le profil est invalide, on reste en Boot : aucun mouvement)
   ▼
Homing   : drivers ON ; chaque axe exécute son HomingController (non bloquant,
   │        en parallèle). L'origine est ancrée sur le capteur HOME (0 mm).
   │        Un axe en défaut est désactivé sans bloquer les autres.
   ▼
Ready    : tous les axes homés → arm() → les notes MIDI sont jouées.
```

Pendant `Boot` et `Homing`, les `Note On` ne sont pas joués (seules les requêtes
SysEx sont traitées). Un changement de configuration mécanique depuis
l'interface Web relance un homing avant de rejouer.

### Arrêt d'urgence matériel et fins de course

* **E-stop matériel** : si une broche `ESTOP` est affectée (active bas), `loop()`
  la lit à chaque passage et déclenche immédiatement un panic (drivers coupés,
  servos neutralisés). Sans broche `ESTOP` affectée, seul le panic logiciel
  (bouton STOP Web / CC120/CC123) est disponible.
* **Fins de course `LIMIT`** : un `LIMIT` actif pendant un déplacement provoque
  un **arrêt immédiat** de l'axe concerné (pas une décélération), invalide sa
  position (re-homing obligatoire) et le met en défaut, sans perturber les
  autres axes.

### Reprise après panic / E-stop (`POST /api/reset`)

Après un panic ou un E-stop, l'état de sécurité est **verrouillé** : ni le
chargement d'un profil ni un nouveau homing ne peuvent réactiver les moteurs.
La reprise est explicite via `POST /api/reset` (bouton « Reset & re-home » du
tableau de bord), acceptée uniquement si :

* l'E-stop est physiquement relâché ;
* aucun `LIMIT` n'est actif ;
* le profil est valide ;
* tous les axes/servos ont pu attacher leur canal matériel.

La reprise force alors un **nouveau homing** avant de rejouer.

### Mode dégradé (`readyDegraded`)

Si un ou plusieurs axes échouent leur homing, le système passe malgré tout en
lecture **mais** :

* les cordes défaillantes sont désactivées (aucune note, même en sélection CC) ;
* la **polyphonie annoncée** par SysEx est réduite au nombre d'axes
  fonctionnels et la **révision des capacités** est incrémentée (General-Midi-
  Boop cesse d'envoyer les notes injouables) ;
* l'état exposé devient `readyDegraded` et le défaut apparaît dans le tableau
  de bord.

### Secrets Wi-Fi et accès

* Les mots de passe Wi-Fi (station et point d'accès) sont stockés en **NVS**
  (`Preferences`), jamais dans le profil exportable, et se règlent via
  `POST /api/wifi`. Le point d'accès peut être protégé en WPA2 (mot de passe ≥ 8
  caractères) ; sinon il reste ouvert.
### Authentification de l'API Web

Les routes qui **déplacent la mécanique ou changent la configuration**
(`PUT /api/profile`, `/api/profiles*`, `/api/reset`, `/api/test/note`,
`/api/test/servo`, `/api/wifi`) sont protégées par un **jeton administrateur**
stocké en NVS :

* tant qu'aucun jeton n'est défini (premier démarrage), les écritures sont
  autorisées pour permettre la configuration initiale ;
* une fois défini via `POST /api/auth`, chaque écriture doit fournir l'en-tête
  `X-GMB-Token` correspondant ; sinon la requête est refusée (401) ;
* `POST /api/panic` reste **toujours** accessible (sécurité) ;
* le statut expose `authConfigured` et `apOpen` pour que l'interface avertisse
  quand le point d'accès est ouvert **et** sans jeton.

**Recommandation** : sur un réseau non maîtrisé, définir un mot de passe AP
(WPA2, ≥ 8 caractères) **et** un jeton administrateur.

**Limitations connues restantes** : pas encore de protection CSRF/origine
dédiée, ni de confirmation physique locale pour le reset/homing, ni de
séparation formelle réseau MIDI / réseau d'administration — voir la note de
limitations du [`README`](../README.md).

---

## 2. Arrêt d'urgence matériel — /OE du PCA9685 (§21.2)

Un arrêt **matériel** doit pouvoir :

* désactiver les drivers pas à pas ;
* désactiver le PCA9685 par sa broche `/OE` (neutralisation immédiate de tous les
  servos, indépendamment du firmware) ;
* neutraliser les sorties auxiliaires ;
* **conserver l'ESP32 alimenté** (pour journalisation et reprise contrôlée).

La sortie `/OE` du PCA9685 est reliée à une broche de sécurité (GPIO47 dans le
profil DevKitC-1 recommandé — voir [`PIN_CONFIGURATION.md`](PIN_CONFIGURATION.md)).
`emergencyStop(nowMs)` verrouille l'état `EmergencyStop` et enregistre la cause.

---

## 3. Panic logiciel (§21.3)

`panic(cause, nowMs)` verrouille l'état `Panic`, enregistre la cause, et le
firmware doit :

* vider la file MIDI ;
* annuler tous les mouvements ;
* annuler tous les pincements ;
* relever les doigts ;
* neutraliser les servos ;
* désactiver les moteurs ;
* enregistrer la cause.

Le mécanisme d'identifiant de commande de `StringController` garantit qu'aucune
attaque différée n'est exécutée après un panic (voir [`ARCHITECTURE.md`](ARCHITECTURE.md)
§3 et `cahier des charges.md` §16). `reset()` ramène en `PowerOnSafe` et exige un
ré-armement.

L'API Web expose `POST /api/panic` ([`WEB_INTERFACE.md`](WEB_INTERFACE.md)).

---

## 4. Perte du Wi-Fi (§21.4)

Comportement configurable (`WifiLossBehavior`) :

| Valeur | Comportement |
| ------ | ------------ |
| `FinishThenStop` (0) | **défaut** : annuler les commandes en attente, relâchement contrôlé, retour à READY |
| `StopImmediately` (1) | arrêter immédiatement |
| `ContinueQueued` (2) | continuer les commandes déjà en file |
| `IdleKeepMotors` (3) | revenir en attente sans désactiver les moteurs |

Comportement par défaut détaillé : annulation des commandes en attente,
relâchement contrôlé, retour à l'état READY.

---

## 5. Journal des défauts

`SafetyManager` intègre le rôle de `FaultManager` (§23) :

```cpp
struct FaultRecord { std::string source; std::string message; uint32_t atMs; };

void recordFault(source, msg, nowMs);
const std::vector<FaultRecord>& faults() const;
void clearFaults();
```

Les défauts sont affichés sur le tableau de bord Web (§19).

---

## 6. Alimentation (§22)

Rails recommandés :

| Rail | Usage |
| ---- | ----- |
| 24 V | moteurs pas à pas |
| 5 à 7,4 V | servomoteurs |
| 5 V | logique |
| 3,3 V | ESP32-S3 |

Exigences :

* alimentation servo **séparée** ;
* fusible moteurs ; fusible servos ;
* protection contre l'inversion de polarité ;
* TVS sur le rail moteur ;
* condensateurs près des drivers ; condensateur de réserve près du PCA9685 ;
* masse commune structurée ;
* connecteurs verrouillables ;
* **aucun servo alimenté par le régulateur de l'ESP32**.
