# Guide de première configuration — Stepper-Plucked-Strings-GMB

> Source : `cahier des charges.md` §8, §10, §26 (guide de première configuration).
> Documents liés : [`WEB_INTERFACE.md`](WEB_INTERFACE.md) · [`PIN_CONFIGURATION.md`](PIN_CONFIGURATION.md) · [`CALIBRATION.md`](CALIBRATION.md) · [`MIDI_PROTOCOL.md`](MIDI_PROTOCOL.md) · [`SAFETY.md`](SAFETY.md).

Ce guide accompagne un débutant du premier démarrage jusqu'à la première note,
en utilisant uniquement le **mode simplifié** de l'interface Web. Aucune
modification de code n'est nécessaire.

---

## 0. Avant de commencer

* Alimentez la carte et les moteurs selon les rails recommandés (voir
  [`SAFETY.md`](SAFETY.md) §6). **N'alimentez jamais les servos par le régulateur
  de l'ESP32.**
* Au démarrage, le système est en état sûr : drivers désactivés, servos
  neutralisés, files MIDI vides (voir [`SAFETY.md`](SAFETY.md) §1). Rien ne bouge
  tant que la configuration n'est pas validée.

---

## 1. Se connecter à l'interface

À la première mise sous tension, l'ESP32 démarre en **mode point d'accès** :

```text
SSID par défaut : Stepper-Plucked-Strings-GMB
```

1. Connectez votre téléphone/ordinateur à ce réseau Wi-Fi.
2. Ouvrez l'adresse locale affichée (ou le portail captif).
3. L'assistant de configuration s'ouvre.

Vous pourrez plus tard basculer en **mode client** (l'ESP32 rejoint votre réseau) :
SSID, mot de passe, nom réseau, IP fixe optionnelle, nom mDNS. Si la connexion
échoue plusieurs fois, le système revient automatiquement en point d'accès.

---

## 2. Étape 1 — Identification

Renseignez : nom de l'instrument, description (optionnelle), **nombre de cordes**
(1 à 6), type d'instrument (ukulélé, guitare, basse, mandoline, banjo…), accordage
proposé, nombre maximal de frettes. Ces valeurs déterminent la plage de notes et
sont annoncées à General-Midi-Boop (voir [`MIDI_PROTOCOL.md`](MIDI_PROTOCOL.md) §3).

---

## 3. Étape 2 — Choix de la carte

Sélectionnez le modèle (**ESP32-S3-DevKitC-1** pris en charge par défaut), la
révision, la Flash, la présence de PSRAM et la variante. Le profil de carte fixe
automatiquement les GPIO disponibles, réservés, recommandés et à utiliser avec
précaution (voir [`PIN_CONFIGURATION.md`](PIN_CONFIGURATION.md)).

> Attention : sur certaines variantes DevKitC-1, GPIO35/36/37 servent à la
> Flash/PSRAM et ne sont pas proposées sans vérification de la variante.

---

## 4. Étape 3 — Attribution automatique des broches

Cliquez sur **« Attribuer automatiquement les broches »**. Le système choisit une
configuration sans conflit selon le nombre de cordes, les interfaces activées, la
carte, la réservation de l'USB futur (GPIO19/20), le port de diagnostic (UART), l'I²C
et les capteurs. En mode simplifié, vous ne voyez que les broches **vertes**. Si un
signal ne peut pas être placé, l'assistant l'explique et suggère une alternative.

Exemple d'attribution obtenue (profil DevKitC-1, voir
[`PIN_CONFIGURATION.md`](PIN_CONFIGURATION.md) §5) : STEP sur 4/5/6…, DIR sur 17/18…,
HOME sur 12/13…, I²C SDA 40 / SCL 41, ENABLE 42, sécurité PCA9685 47.

---

## 5. Étape 4 — Configuration mécanique

Pour chaque corde : note MIDI à vide, frettes max, longueur vibrante, type de
transmission (courroie GT2 / vis / personnalisé), pas moteur par tour,
microstepping, déplacement par tour, inversion du sens, vitesse et accélération
max, position de repos. L'interface **calcule automatiquement les pas/mm**
(formules courroie/vis dans [`CALIBRATION.md`](CALIBRATION.md) §1).

---

## 6. Étape 5 — Homing

Pour chaque axe : capteur activé, GPIO du capteur, contact NO/NC, niveau actif,
direction du homing, vitesses rapide/lente, distance de recul, offset après
origine, timeout, distance maximale de recherche. Le homing est non bloquant et
indépendant par corde (machine d'état `CHECK_SENSOR → … → READY`, voir
[`CALIBRATION.md`](CALIBRATION.md) §2).

---

## 7. Étape 6 — Calibration des servos

Pour chaque servo : canal PCA9685, position de repos, position active, limites
min/max, sens inversé, temps de déplacement, temps de stabilisation, désactivation
au repos. Répartition typique des canaux : doigts 0–5, pincement 6–11, auxiliaires
12–15 (voir [`CALIBRATION.md`](CALIBRATION.md) §4).

---

## 8. Étape 7 — Calibration des notes

Deux méthodes :

* **Calcul automatique des frettes** : `position = longueur vibrante × (1 − 2^(−fret/12))`.
* **Calibration manuelle** : pour chaque frette, déplacez le moteur avec les
  boutons, testez la note, ajustez, enregistrez la position exacte. La table
  calibrée a **priorité** sur la théorie (voir [`CALIBRATION.md`](CALIBRATION.md) §3).

---

## 9. Étape 8 — Test

Testez progressivement : chaque moteur, chaque capteur, chaque doigt, chaque
médiator, chaque note, chaque corde, un accord, puis l'**arrêt général** (STOP).
Gardez le bouton STOP à portée (panic logiciel — voir [`SAFETY.md`](SAFETY.md) §3).

---

## 10. Étape 9 — Validation

L'interface affiche **« Configuration valide »** ou la liste précise des problèmes.
Aucun actionneur n'est activé en mode normal tant que les erreurs critiques ne sont
pas corrigées. Une fois valide, la configuration est sauvegardée (profil), et les
capacités sont publiées vers General-Midi-Boop.

---

## 11. Connecter General-Midi-Boop (optionnel)

Sur la page « MIDI > Identité et capacités GMB », appliquez le préréglage
**General-Midi-Boop** (CC20 = corde, CC21 = frette, mode hybride). GMB découvre
alors automatiquement l'instrument (identité, capacités, cordes) par SysEx. Voir
[`MIDI_PROTOCOL.md`](MIDI_PROTOCOL.md) §2–3 et [`WEB_INTERFACE.md`](WEB_INTERFACE.md) §3.3.

---

## 12. Sauvegarder et repartir

Enregistrez votre configuration comme profil (au moins 8 emplacements), exportez-la
en JSON pour la conserver, et définissez le profil de démarrage. Le mot de passe
Wi-Fi n'est pas inclus dans les exports ordinaires.

Bonne première note !
