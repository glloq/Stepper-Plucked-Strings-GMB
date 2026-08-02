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
