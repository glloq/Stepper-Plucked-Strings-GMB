# Configuration des GPIO — Stepper-Plucked-Strings-GMB

> Source : `cahier des charges.md` §11 · Code : `firmware/src/core/board/BoardProfile.{h,cpp}`, `PinManager.{h,cpp}`.
> Documents liés : [`ARCHITECTURE.md`](ARCHITECTURE.md) · [`WEB_INTERFACE.md`](WEB_INTERFACE.md) · [`CALIBRATION.md`](CALIBRATION.md).

Le firmware ne doit **jamais** utiliser une liste globale de broches identique
pour toutes les cartes. Chaque carte possède un profil qui décrit les capacités
de chaque GPIO exposé, ce qui permet à l'interface Web de filtrer les choix par
signal et par variante de carte, et au validateur de bloquer les conflits.

---

## 1. Modèle de capacités (§11.1)

Chaque carte fournit un `BoardProfile` ; chaque GPIO y est décrit par une
`PinCapability`.

```cpp
struct BoardProfile {
    std::string identifier;              // ex. "esp32-s3-devkitc-1"
    std::string displayName;
    std::vector<PinCapability> pins;
    const PinCapability* find(int8_t gpio) const;
    std::vector<const PinCapability*> candidatesFor(SignalKind kind) const;
    bool supports(int8_t gpio, SignalKind kind) const;
};

struct PinCapability {
    int8_t gpio = -1;
    bool exposed;            // broché sur le connecteur de la carte
    bool input;
    bool output;
    bool interrupt;
    bool highSpeedOutput;    // adapté à STEP / commutation rapide
    bool internalPullUp;
    bool internalPullDown;
    bool adc;
    bool reserved;           // réservé par politique firmware (ex. futur USB)
    bool strapping;          // broche de strapping (boot)
    bool usb;                // USB-JTAG / USB natif
    bool onboardPeripheral;  // câblé à un périphérique embarqué (LED, UART…)
    PinPreference preference;
    std::string note;        // raison lisible, affichée dans l'UI
};
```

---

## 2. Catégories de couleur (§11.2)

L'énumération `PinPreference` pilote l'affichage :

| `PinPreference` | Couleur | Signification | Accès UI |
| --------------- | ------- | ------------- | -------- |
| `Recommended` (0) | 🟢 Vert | recommandé | visible pour tous (débutant inclus) |
| `Caution` (1) | 🟡 Jaune | utilisable avec précaution | **mode avancé uniquement**, avec explication |
| `Reserved` (2) | 🔴 Rouge | réservé ou incompatible | **non sélectionnable** |
| `Used` (3) | ⚪ Gris | déjà utilisé (état runtime) | non sélectionnable tant qu'affecté |

Règle : un débutant ne voit par défaut que les GPIO **verts**. Les GPIO jaunes
n'apparaissent qu'en mode avancé, avec une explication. Les GPIO rouges ne
peuvent jamais être sélectionnés.

---

## 3. Filtrage par signal (§11.3)

Le type de signal demandé est décrit par `SignalKind`. `BoardProfile::candidatesFor(kind)`
ne renvoie que les broches légales, dans l'ordre de préférence (recommandées
d'abord) ; les broches réservées ou non exposées ne sont jamais proposées.

| `SignalKind` | Exige | Ne propose que des GPIO… |
| ------------ | ----- | ------------------------ |
| `Step` | `output` + `highSpeedOutput` | capables de sortie **rapide**, non réservés, non affectés, compatibles avec le générateur de pas |
| `Dir` | `output` | sortie simple |
| `Enable` | `output` | sortie simple |
| `Home` | `input` + `interrupt` + polarisation | entrée compatible interruptions, avec pull adaptée ou résistance externe, non affectée |
| `Limit` | `input` + `interrupt` + polarisation | idem `Home` (fin de course opposée) |
| `Diag` | `input` | entrée (DIAG TMC2209) |
| `I2cSda` | I²C | broches utilisables en I²C, paire recommandée par défaut, aucune déjà utilisée |
| `I2cScl` | I²C | idem |
| `ServoOe` | `output` | sortie de sécurité vers `/OE` du PCA9685 |
| `Generic` | `output` | toute sortie utilisable |

Pour une interface USB future, GPIO19 et GPIO20 sont automatiquement réservés.

---

## 4. Restrictions ESP32-S3 (§11.4)

Le gestionnaire de broches connaît au minimum ces restrictions :

| GPIO | Restriction | Conséquence par défaut |
| ---- | ----------- | ---------------------- |
| **0, 3, 45, 46** | broches de strapping (boot) | 🔴 réservées |
| **19, 20** | USB-JTAG / USB natif | 🔴 réservées (futur USB) |
| **26 – 32** | normalement liées à la Flash / PSRAM | 🔴 réservées |
| **33 – 37** | peuvent être liées à la mémoire selon la variante | 🔴/🟡 selon variante |
| **35, 36, 37** | Flash/PSRAM sur certaines variantes DevKitC-1 | 🔴 non proposées sans vérification de variante |
| **43, 44** | UART principal (programmation/diagnostic) | 🔴 réservées |
| **48** | LED RGB embarquée (DevKitC-1) | 🔴 réservée |

> Ces broches ne sont pas nécessairement inutilisables dans tous les cas : elles
> doivent être classées selon le **profil exact** de la carte sélectionnée.

---

## 5. Profil recommandé ESP32-S3-DevKitC-1 (§11.5)

`makeEsp32S3DevKitC1()` fournit le profil de référence. L'attribution automatique
(`PinManager::autoAssign`) suit ce plan initial :

| Fonction | GPIO proposés |
| -------- | ------------- |
| STEP 1 à 6 | 4, 5, 6, 7, 15, 16 |
| DIR 1 à 6 | 17, 18, 8, 9, 10, 11 |
| HOME 1 à 6 | 12, 13, 14, 21, 38, 39 |
| I²C SDA | 40 |
| I²C SCL | 41 |
| ENABLE global | 42 |
| Sortie de sécurité PCA9685 (`/OE`) | 47 |

> Ce plan est un **profil logiciel initial**, pas une règle universelle : il peut
> être remplacé depuis l'interface (mode avancé).

### Broches conservées par défaut

| GPIO | Réservation |
| ---- | ----------- |
| 19, 20 | futur USB |
| 43, 44 | programmation et diagnostic UART |
| 0 | démarrage / BOOT |
| 3, 45, 46 | strapping |
| 48 | LED intégrée |
| 35, 36, 37 | dépendance à la variante Flash/PSRAM |

---

## 6. Attribution automatique (moteur `PinManager`)

L'attribution est pilotée par une `PinRequest` :

```cpp
struct PinRequest {
    int stringCount = 1;
    bool useI2cServos = true;   // PCA9685 présent
    bool globalEnable = true;   // une seule ligne ENABLE pour tous les drivers
    bool servoSafetyOe = true;  // /OE du PCA9685 relié à une broche de sécurité
    bool reserveUsb = true;     // conserver GPIO19/20 libres pour l'USB natif
    bool useLimitSwitches = false;
};
```

`autoAssign()` construit une configuration **sans conflit** en suivant le profil
recommandé, et bascule sur n'importe quel candidat compatible lorsqu'une broche
préférée est déjà prise. Il renvoie `false` si un signal requis n'a pas pu être
placé. Chaque affectation est un `PinAssignment { signal, kind, gpio }`
(ex. `"STEP1"`, `"HOME3"`, `"SDA"`).

---

## 7. Détection des conflits (§11.6)

`PinManager::validate(reserveUsb)` renvoie une liste (vide = configuration valide)
de `PinError`. Le validateur empêche :

* deux signaux utilisant le même GPIO ;
* une entrée affectée à une broche indisponible ;
* un signal `STEP` sur une broche non compatible (sortie rapide) ;
* l'utilisation d'un GPIO réservé ;
* l'utilisation de GPIO19/20 lorsque l'USB est réservé ;
* la sélection d'une broche Flash/PSRAM ;
* l'utilisation simultanée de la LED intégrée et du même GPIO ;
* le remplacement involontaire du port de diagnostic (UART).

Chaque erreur est explicite :

```cpp
struct PinError {
    std::string signal;        // ex. "STEP3"
    int8_t gpio;
    std::string reason;        // pourquoi la broche est incompatible
    std::string suggestion;    // quelle broche choisir à la place
    std::string conflictWith;  // quel signal utilise déjà la broche
};
```

Ainsi chaque erreur indique : **pourquoi** la broche est incompatible, **quelle**
broche choisir, et **quelle fonction** occupe déjà la broche.

L'API Web correspondante (`POST /api/pins/auto`, `POST /api/pins/validate`,
`GET /api/board/{id}`) est décrite dans [`WEB_INTERFACE.md`](WEB_INTERFACE.md).
