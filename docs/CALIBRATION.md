# Procédure de calibration — Stepper-Plucked-Strings-GMB

> Sources : `cahier des charges.md` §12, §13, §14, §15 · Code : `core/motion/{StepperAxis.*, HomingController.*}`, `core/Types.*`, `core/configuration/Profile.h`.
> Documents liés : [`WEB_INTERFACE.md`](WEB_INTERFACE.md) (assistant §10) · [`SAFETY.md`](SAFETY.md) · [`FIRST_CONFIGURATION.md`](FIRST_CONFIGURATION.md).

Ce document décrit la calibration : pas/mm du moteur, homing, positions de
frettes (théoriques et manuelles), et servos.

---

## 1. Calcul assisté des pas/mm (§12.1)

Le firmware travaille en **millimètres** et convertit vers les pas moteur via un
facteur `steps/mm` dépendant de la transmission (`StepperAxis::stepsPerMm()`), ce
qui abstrait le type mécanique du reste du code.

```cpp
enum class Transmission { BeltGt2, Screw, Custom };
```

### 1.1 Courroie (GT2)

```text
                stepsPerRevolution × microsteps
stepsPerMm = ──────────────────────────────────
                    pulleyTeeth × beltPitch
```

Exemple : moteur 1,8° (200 pas/tour), 16 microsteps, poulie 20 dents, pas GT2
2 mm → `(200 × 16) / (20 × 2) = 80 pas/mm`.

### 1.2 Vis

```text
                stepsPerRevolution × microsteps
stepsPerMm = ──────────────────────────────────
                    leadPerRevolution
```

Exemple : 200 pas/tour, 16 microsteps, vis à pas de 8 mm/tour → `3200 / 8 = 400 pas/mm`.

### 1.3 Valeur personnalisée

Transmission `Custom` : `customStepsPerMm` est utilisé directement.

Paramètres pertinents dans `AxisConfig` : `stepsPerRevolution`, `microsteps`,
`pulleyTeeth`, `beltPitchMm`, `leadPerRevolutionMm`, `customStepsPerMm`,
`invertDirection`, `minPositionMm`/`maxPositionMm`, `maxSpeedMmS`, `maxAccelMmS2`.
Conversions : `mmToSteps(mm)`, `stepsToMm(steps)`, bornage `clampToLimits(mm)`.

---

## 2. Homing (§13)

Le homing est **non bloquant** et **indépendant** pour chaque corde
(`HomingController`, une instance par axe). À chaque tick, il lit le capteur et la
position, et renvoie la commande de mouvement à appliquer (`HomingCommand`).

### 2.1 Machine d'état

```text
Idle → CheckSensor → SeekFast → (SensorDetected) → Backoff →
SeekSlow → SetZero → MoveToOffset → Ready
                                       └─(défaut)─► Fault
```

| État (`HomingState`) | Rôle |
| -------------------- | ---- |
| `Idle` | inactif |
| `CheckSensor` | vérifier que le capteur n'est pas déjà actif |
| `SeekFast` | approche rapide vers le capteur (`fastSpeedMmS`) |
| `Backoff` | recul après détection (`backoffMm`) |
| `SeekSlow` | ré-approche lente précise (`slowSpeedMmS`) |
| `SetZero` | fixer l'origine |
| `MoveToOffset` | aller à l'offset de repos (`offsetMm`) |
| `Ready` | axe prêt |
| `Fault` | axe désactivé |

Configuration (`HomingConfig`) : `direction` (±1 vers le capteur), `fastSpeedMmS`,
`slowSpeedMmS`, `backoffMm`, `offsetMm`, `timeoutMs` (défaut 8000), `maxSearchMm`
(défaut 500), `sensorActiveHigh` (le niveau électrique brut est normalisé en
interne).

### 2.2 Défauts détectés (§13.2, `HomingFault`)

| Défaut | Cause |
| ------ | ----- |
| `SensorActiveAtStart` | capteur actif au démarrage |
| `SensorNotReleased` | capteur impossible à libérer |
| `SensorNeverReached` | capteur jamais atteint |
| `Timeout` | délai dépassé |
| `MaxDistanceExceeded` | distance maximale de recherche dépassée |
| — | activation incohérente de HOME et LIMIT (détectée en amont) |

Un axe défaillant est désactivé **sans provoquer de mouvement imprévu** sur les
autres axes.

### 2.3 Homing parallèle (§13.1)

Options : **simultané**, **séquentiel** (si l'alimentation est limitée), ou **par
groupes**. Chaque axe garde sa propre instance de `HomingController`.

---

## 3. Calibration des notes / frettes (§14)

### 3.1 Accordage

Chaque corde possède : note MIDI à vide (`openNote`), frette maximale incluse
(`maxFret`), position de chaque frette. Accordages prédéfinis proposés (guitare,
basse, ukulélé, mandoline, banjo, personnalisé), entièrement modifiables.

### 3.2 Calcul théorique (§14.2)

```text
position = longueur vibrante × (1 − 2^(−fret / 12))
```

Implémenté dans `core/Types.cpp` (`fretPositionMm(scaleLengthMm, fret)`) et exposé
par `StepperAxis::fretPositionMm(fret)`. `scaleLengthMm` = longueur vibrante de la
corde. La note produite à une frette : `note = openNote + fret + capo + transpose`.

Exemple (longueur 330 mm) : frette 12 → `330 × (1 − 2^(−1)) = 165 mm` (octave à la
moitié de la corde).

### 3.3 Calibration manuelle (§14.3)

Pour chaque frette : (1) sélectionner la frette, (2) déplacer le moteur avec des
boutons, (3) tester la note, (4) ajuster la position, (5) enregistrer la position
exacte. **La table calibrée a priorité sur la position théorique** : si
`AxisConfig::calibratedFretMm[fret]` est renseigné, `fretPositionMm()` renvoie la
valeur calibrée plutôt que la théorique.

### 3.4 Compensation (§14.4)

Le système permet : correction individuelle d'une frette, correction selon le sens
de déplacement (jeu mécanique / backlash), offset global de la corde, limites
logicielles avant et arrière (`minPositionMm` / `maxPositionMm`, appliquées par
`clampToLimits`).

---

## 4. Calibration des servos (§15)

Chaque servo utilise des impulsions calibrées en microsecondes (`ServoConfig`) :

```cpp
struct ServoConfig {
    bool enabled;
    uint8_t channel;              // canal PCA9685
    std::string function;         // "finger" / "pluck" / "damper" / "aux"
    uint16_t pulseMinUs, pulseMaxUs;
    uint16_t restUs, activeUs;    // position de repos / active
    bool inverted;
    uint16_t travelMs;            // temps de déplacement
    uint16_t settleMs;            // temps de stabilisation
    bool disableAtRest;           // désactivation au repos
};
```

### 4.1 Doigt (§15.1)

Positions relevée / appuyée, délai après appui, délai après relâchement. La corde
à vide (frette 0) : doigt relevé, moteur éventuellement en position de sécurité,
pincement direct autorisé.

### 4.2 Médiator individuel (§15.2)

Positions gauche / droite / repos, alternance automatique, course min/max, vitesse
ou délai de mouvement.

### 4.3 Corde à vide (§15.3)

Doigt relevé, moteur éventuellement déplacé vers une position de sécurité,
pincement autorisé directement. Option avancée : utiliser le doigt sur le fret
zéro pour une mécanique spécifique.

Répartition recommandée des 16 canaux PCA9685 : 0–5 appui des doigts, 6–11
pincement individuel, 12–15 étouffoirs / grattage partagé / auxiliaires. La sortie
`/OE` du PCA9685 est reliée à une broche de sécurité — voir [`SAFETY.md`](SAFETY.md).
