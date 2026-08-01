# Cahier des charges — Stepper-Plucked-Strings-GMB

**Version :** 1.0
**Statut :** spécification initiale
**Plateforme cible :** ESP32-S3
**Nombre de cordes :** 1 à 6
**Communication initiale :** Wi-Fi
**Configuration :** interface Web locale
**Type d’instruments :** instruments à cordes pincées ou grattées

---

# 1. Objet du projet

Stepper-Plucked-Strings-GMB est un contrôleur MIDI modulaire destiné aux instruments à cordes pincées ou grattées.

Le système doit déplacer un doigt mécanique unique le long de chaque corde afin de sélectionner la note à jouer.

Chaque corde dispose de son propre axe :

```text
Corde 1 → moteur pas à pas 1 → doigt mobile 1
Corde 2 → moteur pas à pas 2 → doigt mobile 2
Corde 3 → moteur pas à pas 3 → doigt mobile 3
...
Corde 6 → moteur pas à pas 6 → doigt mobile 6
```

Pour chaque corde :

```text
1 moteur pas à pas
1 axe linéaire
1 chariot
1 doigt mobile
1 mécanisme d’appui du doigt
1 mécanisme de pincement
1 capteur de référence
```

Le moteur pas à pas assure exclusivement le déplacement longitudinal du doigt.

L’appui, le pincement et l’étouffement peuvent être assurés par des servomoteurs ou d’autres actionneurs auxiliaires, mais ils ne remplacent pas le moteur pas à pas utilisé pour sélectionner la note.

---

# 2. Positionnement dans la famille GMB

Le projet doit rester spécialisé afin d’éviter un firmware universel trop complexe.

Les technologies de sélection des notes seront réparties dans des projets distincts :

```text
Stepper-Plucked-Strings-GMB
└── un moteur pas à pas déplace un doigt unique par corde

Servo-Plucked-Strings-GMB
└── plusieurs servomoteurs fixes actionnent différentes positions

Solenoid-Plucked-Strings-GMB
└── plusieurs solénoïdes fixes actionnent différentes positions
```

Le présent cahier des charges concerne uniquement :

```text
Stepper-Plucked-Strings-GMB
```

Une base commune pourra ultérieurement être extraite pour :

* le traitement MIDI ;
* la communication ;
* la configuration Web ;
* la gestion des profils ;
* les diagnostics.

La logique mécanique de chaque projet doit néanmoins rester indépendante.

---

# 3. Instruments ciblés

Le système doit pouvoir être adapté à :

* ukulélé ;
* guitare ;
* basse ;
* mandoline ;
* banjo ;
* guitare ténor ;
* cithare ;
* instruments expérimentaux à cordes pincées ;
* instruments utilisant un médiator individuel ;
* instruments utilisant un système de grattage partagé.

Le projet ne doit pas imposer :

* un accordage précis ;
* un nombre fixe de cordes ;
* un nombre fixe de frettes ;
* une longueur vibrante unique ;
* un modèle unique de transmission ;
* un type unique de servomoteur ;
* un câblage fixe des GPIO.

---

# 4. Fonctions exclues

Cette version ne doit pas gérer :

* les instruments à cordes frottées ;
* les archets linéaires ;
* les roues d’archet ;
* les moteurs DC de frottement ;
* les moteurs BLDC ;
* la régulation de vitesse d’un archet ;
* une matrice de doigts fixes à servomoteurs ;
* une matrice de doigts fixes à solénoïdes ;
* plusieurs doigts mobiles sur une même corde ;
* un moteur pas à pas partagé entre plusieurs cordes.

---

# 5. Architecture mécanique de référence

## 5.1 Canal de corde

Chaque corde constitue un canal indépendant.

```text
Moteur pas à pas
        ↓
Transmission mécanique
        ↓
Chariot longitudinal
        ↓
Doigt unique
        ↓
Position sur la corde
```

La transmission peut utiliser :

* courroie GT2 ;
* vis trapézoïdale ;
* vis à billes ;
* crémaillère ;
* câble ;
* mécanisme expérimental.

Le firmware doit utiliser une unité physique indépendante du type de transmission :

```text
position en millimètres
        ↓
conversion
        ↓
position en pas moteur
```

## 5.2 Appui du doigt

Une fois le chariot positionné, le doigt doit pouvoir :

* descendre sur la corde ;
* maintenir la corde ;
* se relever ;
* rester relevé pendant les déplacements ;
* rester relevé pour une corde à vide.

Le mécanisme de référence utilise un servomoteur par corde.

## 5.3 Mise en vibration

Deux modes doivent être prévus.

### Pincement individuel

Chaque corde possède son propre actionneur de pincement.

```text
1 servo de pincement par corde
```

Ce mode permet :

* accords simultanés ;
* notes répétées ;
* tremolo individuel ;
* contrôle individuel de la vélocité ;
* déclenchement précis de chaque corde.

### Grattage partagé

Un mécanisme commun traverse plusieurs cordes.

Il doit permettre :

* grattage montant ;
* grattage descendant ;
* vitesse réglable ;
* plage de cordes réglable ;
* retour en position de repos ;
* exclusion de certaines cordes ;
* synchronisation avec les doigts.

Le même instrument peut combiner des pincements individuels et un grattage partagé.

---

# 6. Capacité cible

| Ressource                        | Minimum |   Maximum |
| -------------------------------- | ------: | --------: |
| Cordes                           |       1 |         6 |
| Moteurs pas à pas                |       1 |         6 |
| Doigts mobiles                   |       1 |         6 |
| Capteurs de référence            |       1 |         6 |
| Fins de course opposées          |       0 |         6 |
| Servos d’appui                   |       1 |         6 |
| Servos de pincement              |       0 |         6 |
| Servos auxiliaires               |       0 |         4 |
| Sorties servo totales            |       1 |        16 |
| Sorties de puissance auxiliaires |       0 |         8 |
| Profils sauvegardés              |       1 | 8 minimum |

La relation suivante doit être conservée :

```text
nombre de cordes actives
=
nombre d’axes pas à pas actifs
=
nombre de doigts mobiles
```

---

# 7. Architecture électronique

```text
                         Wi-Fi
                           │
               MIDI + configuration Web
                           │
                           ▼
                       ESP32-S3
                           │
        ┌──────────────────┼──────────────────┐
        │                  │                  │
 Commande STEP/DIR        I²C              Capteurs
        │                  │                  │
 1 à 6 drivers          PCA9685          HOME / LIMIT
        │                  │
 1 à 6 moteurs       1 à 16 servos
```

## 7.1 Contrôleur principal

La plateforme de référence doit utiliser un ESP32-S3.

Le contrôleur doit assurer :

* réception des commandes MIDI par Wi-Fi ;
* hébergement de l’interface Web ;
* allocation des notes ;
* génération des trajectoires ;
* gestion des machines d’état ;
* commande du PCA9685 ;
* surveillance des capteurs ;
* stockage des profils ;
* diagnostic ;
* sécurité.

L’ESP32-S3 dispose d’une matrice GPIO permettant de router de nombreux signaux périphériques vers différents GPIO. Cette flexibilité permet d’utiliser des profils de cartes et une attribution configurable des broches.

## 7.2 Drivers pas à pas

Le driver de référence doit être le TMC2209 ou un driver compatible STEP/DIR.

Chaque axe doit disposer de :

```text
STEP
DIR
ENABLE
HOME
LIMIT optionnel
DIAG optionnel
UART optionnel
```

La première carte de prototype doit accepter des modules de drivers enfichables.

Cela facilite :

* le remplacement d’un driver ;
* les essais avec plusieurs modèles ;
* la maintenance ;
* l’adaptation du courant moteur ;
* le prototypage avant création d’un PCB intégré.

## 7.3 Servomoteurs

Un PCA9685 doit fournir jusqu’à 16 sorties servo.

Répartition recommandée :

| Canaux  | Utilisation                                           |
| ------- | ----------------------------------------------------- |
| 0 à 5   | appui des doigts                                      |
| 6 à 11  | pincement individuel                                  |
| 12 à 15 | étouffoirs, grattage partagé ou fonctions auxiliaires |

La sortie `OE` du PCA9685 doit être reliée à une broche de sécurité afin de neutraliser immédiatement les servos.

---

# 8. Communication

## 8.1 Version initiale : Wi-Fi

La première version doit fonctionner exclusivement par Wi-Fi pour les communications externes.

Deux modes réseau doivent être proposés.

### Mode point d’accès

L’ESP32 crée son propre réseau Wi-Fi.

```text
SSID par défaut :
Stepper-Plucked-Strings-GMB

Adresse de configuration :
adresse locale affichée ou portail captif
```

Ce mode doit permettre une première configuration sans routeur.

### Mode client Wi-Fi

L’ESP32 rejoint le réseau local de l’utilisateur.

Le système doit enregistrer :

* SSID ;
* mot de passe ;
* nom réseau de l’instrument ;
* adresse fixe optionnelle ;
* nom mDNS ;
* paramètres de reconnexion.

Si la connexion échoue plusieurs fois, le système doit revenir automatiquement en mode point d’accès.

## 8.2 Transport MIDI initial

La couche de transport doit être séparée du moteur MIDI interne.

Les entrées Wi-Fi pourront comprendre :

* WebSocket binaire ;
* RTP-MIDI ;
* protocole UDP configurable ;
* commandes de test depuis l’interface Web.

Tous les transports doivent produire un événement interne commun :

```cpp
struct MidiEvent {
    uint32_t timestampUs;
    uint8_t source;
    uint8_t type;
    uint8_t channel;
    uint8_t data1;
    uint8_t data2;
};
```

## 8.3 Extensions futures

L’architecture doit permettre d’ajouter ultérieurement :

```text
BLE MIDI
USB MIDI
MIDI DIN
liaison série
CAN ou RS485
```

L’ajout d’un nouveau transport ne doit pas modifier :

* le contrôleur des cordes ;
* l’allocateur de notes ;
* la gestion des mouvements ;
* les profils mécaniques.

Les GPIO19 et GPIO20 doivent rester réservés par défaut afin de conserver la possibilité d’utiliser ultérieurement l’USB natif de l’ESP32-S3. Ces broches sont utilisées par l’interface USB-JTAG/USB native du composant.

---

# 9. Interface Web

## 9.1 Objectif

L’interface doit permettre à un débutant de configurer l’instrument sans modifier le code source.

Elle doit fonctionner depuis :

* ordinateur ;
* tablette ;
* téléphone.

Aucune application dédiée ne doit être nécessaire.

## 9.2 Deux niveaux d’interface

### Mode simplifié

Destiné aux débutants.

Il doit proposer :

* assistant étape par étape ;
* valeurs recommandées ;
* attribution automatique des broches ;
* schémas de branchement ;
* boutons de test ;
* validation automatique ;
* messages d’erreur compréhensibles.

### Mode avancé

Destiné à la mise au point.

Il doit permettre :

* attribution manuelle des GPIO ;
* réglage des vitesses ;
* réglage des accélérations ;
* réglage des délais ;
* modification des courbes de vélocité ;
* accès aux diagnostics ;
* édition des paramètres détaillés ;
* import et export JSON.

---

# 10. Assistant de première configuration

L’interface doit guider l’utilisateur dans l’ordre suivant.

## Étape 1 — Identification

* nom de l’instrument ;
* description optionnelle ;
* nombre de cordes ;
* type d’instrument ;
* accordage proposé ;
* nombre maximal de frettes.

## Étape 2 — Choix de la carte

L’utilisateur sélectionne :

* modèle de carte ESP32 ;
* révision ;
* capacité Flash ;
* présence de PSRAM ;
* variante du module.

Le profil de carte détermine automatiquement :

* les GPIO disponibles ;
* les GPIO réservés ;
* les GPIO recommandés ;
* les GPIO à utiliser avec précaution ;
* les fonctions présentes sur la carte.

Le profil initial doit prendre en charge :

```text
ESP32-S3-DevKitC-1
```

La carte DevKitC-1 expose la majorité des GPIO du module, mais certaines variantes utilisent GPIO35, GPIO36 et GPIO37 pour la Flash ou la PSRAM interne. Ces broches ne doivent donc pas être proposées sans vérification de la variante sélectionnée.

## Étape 3 — Attribution automatique

Le bouton suivant doit être proposé :

```text
Attribuer automatiquement les broches
```

Le système choisit une configuration sans conflit en fonction :

* du nombre de cordes ;
* des interfaces activées ;
* de la carte sélectionnée ;
* de l’utilisation future de l’USB ;
* du besoin de conserver le port de diagnostic ;
* des périphériques I²C ;
* du nombre de capteurs.

## Étape 4 — Configuration mécanique

Pour chaque corde :

* note MIDI à vide ;
* nombre maximal de frettes ;
* longueur vibrante ;
* type de transmission ;
* pas moteur par tour ;
* microstepping ;
* déplacement par tour ;
* inversion du sens ;
* vitesse maximale ;
* accélération maximale ;
* position de repos.

## Étape 5 — Homing

Pour chaque axe :

* capteur activé ;
* GPIO du capteur ;
* contact NO ou NC ;
* niveau actif ;
* direction du homing ;
* vitesse rapide ;
* vitesse lente ;
* distance de recul ;
* offset après origine ;
* timeout ;
* limite maximale de recherche.

## Étape 6 — Calibration des servos

Pour chaque servo :

* canal PCA9685 ;
* position de repos ;
* position active ;
* limites minimale et maximale ;
* sens inversé ;
* temps de déplacement ;
* temps de stabilisation ;
* désactivation au repos.

## Étape 7 — Calibration des notes

Deux méthodes doivent être proposées :

```text
Calcul automatique des frettes
Calibration manuelle de chaque position
```

## Étape 8 — Test

L’utilisateur doit pouvoir tester :

* chaque moteur ;
* chaque capteur ;
* chaque doigt ;
* chaque médiator ;
* chaque note ;
* chaque corde ;
* un accord ;
* l’arrêt général.

## Étape 9 — Validation

L’interface affiche :

```text
Configuration valide
```

ou une liste précise des problèmes.

Aucun actionneur ne doit être activé en mode normal tant que les erreurs critiques ne sont pas corrigées.

---

# 11. Gestion configurable des GPIO

## 11.1 Principe

Le firmware ne doit pas utiliser une liste globale identique pour toutes les cartes.

Chaque carte doit posséder un profil :

```cpp
struct BoardProfile {
    const char* identifier;
    const char* displayName;
    PinCapability pins[MAX_BOARD_PINS];
};
```

Chaque GPIO doit être décrit par des capacités :

```cpp
struct PinCapability {
    int8_t gpio;
    bool exposed;
    bool input;
    bool output;
    bool interrupt;
    bool highSpeedOutput;
    bool internalPullUp;
    bool internalPullDown;
    bool adc;
    bool reserved;
    bool strapping;
    bool usb;
    bool onboardPeripheral;
    PinPreference preference;
};
```

## 11.2 Catégories affichées

Dans l’interface :

```text
Vert    → recommandé
Jaune   → utilisable avec précaution
Rouge   → réservé ou incompatible
Gris    → déjà utilisé
```

Un débutant ne doit voir par défaut que les GPIO recommandés.

Les GPIO jaunes ne doivent être accessibles qu’en mode avancé, avec une explication.

Les GPIO rouges ne doivent pas pouvoir être sélectionnés.

## 11.3 Liste filtrée selon l’usage

Lors de la configuration d’un signal `STEP`, la liste doit uniquement proposer les GPIO :

* capables de fonctionner en sortie ;
* adaptés aux signaux rapides ;
* non réservés ;
* non affectés ;
* compatibles avec le générateur de pas.

Pour `HOME` ou `LIMIT`, la liste doit uniquement proposer les GPIO :

* capables de fonctionner en entrée ;
* compatibles avec les interruptions ;
* possédant une polarisation adaptée ou utilisant une résistance externe ;
* non affectés.

Pour `SDA` et `SCL`, la liste doit proposer :

* les GPIO utilisables par l’I²C ;
* une paire recommandée par défaut ;
* aucune broche déjà utilisée.

Pour une interface future USB, GPIO19 et GPIO20 doivent être automatiquement réservés.

## 11.4 Restrictions ESP32-S3

Le gestionnaire de broches doit connaître au minimum les restrictions suivantes :

* GPIO0, GPIO3, GPIO45 et GPIO46 sont des broches de strapping ;
* GPIO19 et GPIO20 sont utilisés par l’USB-JTAG/USB natif ;
* GPIO26 à GPIO32 sont normalement liés à la Flash ou à la PSRAM ;
* GPIO33 à GPIO37 peuvent également être utilisés par la mémoire sur certaines variantes ;
* GPIO48 commande la LED RGB sur la DevKitC-1 ;
* GPIO43 et GPIO44 sont liés au port UART principal de la DevKitC-1.

Ces broches ne sont pas nécessairement inutilisables dans tous les cas, mais elles doivent être classées selon le profil exact de la carte.

## 11.5 Profil conseillé pour ESP32-S3-DevKitC-1

Exemple initial d’attribution automatique :

| Fonction                   | GPIO proposés          |
| -------------------------- | ---------------------- |
| STEP 1 à 6                 | 4, 5, 6, 7, 15, 16     |
| DIR 1 à 6                  | 17, 18, 8, 9, 10, 11   |
| HOME 1 à 6                 | 12, 13, 14, 21, 38, 39 |
| I²C SDA                    | 40                     |
| I²C SCL                    | 41                     |
| ENABLE global              | 42                     |
| Sortie de sécurité PCA9685 | 47                     |

Cette attribution constitue un profil logiciel initial et non une règle universelle.

Elle doit pouvoir être remplacée depuis l’interface.

Broches conservées par défaut :

| GPIO       | Réservation                          |
| ---------- | ------------------------------------ |
| 19, 20     | futur USB                            |
| 43, 44     | programmation et diagnostic UART     |
| 0          | démarrage/BOOT                       |
| 3, 45, 46  | strapping                            |
| 48         | LED intégrée                         |
| 35, 36, 37 | dépendance à la variante Flash/PSRAM |

## 11.6 Détection des conflits

Le validateur doit empêcher :

* deux signaux utilisant le même GPIO ;
* une entrée affectée à une broche indisponible ;
* un signal STEP sur une broche non compatible ;
* l’utilisation d’un GPIO réservé ;
* l’utilisation de GPIO19 ou GPIO20 lorsque l’USB est réservé ;
* la sélection d’une broche Flash/PSRAM ;
* l’utilisation simultanée d’une LED intégrée et du même GPIO ;
* le remplacement involontaire du port de diagnostic.

Chaque erreur doit expliquer :

```text
pourquoi la broche est incompatible
quelle broche choisir à la place
quelle fonction utilise déjà la broche
```

---

# 12. Configuration des moteurs pas à pas

Chaque axe doit posséder les paramètres suivants :

```text
axe activé
GPIO STEP
GPIO DIR
GPIO ENABLE ou groupe ENABLE
GPIO HOME
GPIO LIMIT optionnel
GPIO DIAG optionnel
sens moteur inversé
niveau ENABLE inversé
pas moteur par tour
microstepping
déplacement mécanique par tour
pas par millimètre
vitesse maximale
accélération maximale
vitesse de homing rapide
vitesse de homing lente
distance de recul
offset de référence
position minimale
position maximale
délai de désactivation
courant moteur indicatif
```

## 12.1 Calcul assisté

L’interface doit calculer automatiquement les pas par millimètre.

### Courroie

```text
stepsPerMm =
stepsPerRevolution × microsteps
────────────────────────────
pulleyTeeth × beltPitch
```

### Vis

```text
stepsPerMm =
stepsPerRevolution × microsteps
────────────────────────────
leadPerRevolution
```

L’utilisateur doit pouvoir sélectionner :

```text
Courroie GT2
Vis
Valeur personnalisée
```

---

# 13. Homing

Le homing doit être non bloquant et indépendant pour chaque corde.

```text
CHECK_SENSOR
      ↓
SEEK_FAST
      ↓
SENSOR_DETECTED
      ↓
BACKOFF
      ↓
SEEK_SLOW
      ↓
SET_ZERO
      ↓
MOVE_TO_OFFSET
      ↓
READY
```

## 13.1 Homing parallèle

Les moteurs peuvent effectuer leur homing simultanément.

Une option doit permettre :

```text
Homing simultané
Homing séquentiel
Homing par groupes
```

Le mode séquentiel peut être utilisé lorsque l’alimentation est limitée.

## 13.2 Défauts détectés

* capteur actif au démarrage ;
* capteur impossible à libérer ;
* capteur jamais atteint ;
* capteur instable ;
* timeout ;
* distance maximale dépassée ;
* activation incohérente de HOME et LIMIT.

Un axe défaillant doit être désactivé sans provoquer de mouvement imprévu sur les autres axes.

---

# 14. Configuration des notes

## 14.1 Accordage

Chaque corde possède :

```text
note MIDI à vide
fret maximal inclus
position de chaque fret
```

Le système doit proposer des accordages prédéfinis :

* guitare ;
* basse ;
* ukulélé ;
* mandoline ;
* banjo ;
* configuration personnalisée.

Les accordages prédéfinis doivent rester entièrement modifiables.

## 14.2 Calcul théorique

La position théorique d’un fret est :

```text
position =
longueur vibrante × (1 - 2^(-fret / 12))
```

## 14.3 Calibration manuelle

Pour chaque fret, l’utilisateur doit pouvoir :

1. sélectionner le fret ;
2. déplacer le moteur avec des boutons ;
3. tester la note ;
4. ajuster la position ;
5. enregistrer la position exacte.

La table calibrée doit avoir priorité sur la position théorique.

## 14.4 Compensation

Le système doit permettre :

* correction individuelle d’un fret ;
* correction selon le sens de déplacement ;
* compensation du jeu mécanique ;
* offset global de la corde ;
* limite logicielle avant et arrière.

---

# 15. Configuration des servos

Chaque servo doit utiliser des impulsions calibrées en microsecondes.

Paramètres :

```text
servo activé
canal PCA9685
fonction
impulsion minimale
impulsion maximale
position de repos
position active
position A
position B
sens inversé
temps de déplacement
temps de stabilisation
désactivation au repos
```

## 15.1 Doigt

Le servo du doigt doit disposer de :

```text
position relevée
position appuyée
délai après appui
délai après relâchement
```

## 15.2 Médiator individuel

Le médiator doit disposer de :

```text
position gauche
position droite
position de repos
alternance automatique
course minimale
course maximale
vitesse ou délai de mouvement
```

## 15.3 Corde à vide

Pour une corde à vide :

```text
doigt relevé
moteur éventuellement déplacé vers une position de sécurité
pincement autorisé directement
```

Une option avancée peut permettre d’utiliser le doigt sur le fret zéro pour une mécanique spécifique.

---

# 16. Machines d’état

Chaque corde doit utiliser une machine d’état indépendante.

```text
DISABLED
HOMING
IDLE
RELEASING_FINGER
MOVING
PRESSING_FINGER
SETTLING
READY_TO_PLUCK
PLUCKING
SUSTAINING
DAMPING
CANCELLING
FAULT
```

Aucun `delay()` bloquant ne doit être utilisé pendant le jeu.

Chaque commande doit posséder un identifiant.

Si une commande est annulée ou remplacée, toutes les actions différées associées à son ancien identifiant doivent être ignorées.

Cela empêche :

* un pincement après un Note Off ;
* un appui retardé ;
* l’exécution d’une ancienne position ;
* une attaque après un panic.

---

# 17. Allocation des notes

## 17.1 Principe

Une note doit être attribuée à une corde :

* capable de jouer la note ;
* initialisée ;
* sans défaut ;
* disponible ;
* demandant le temps de préparation le plus faible.

## 17.2 Accords

Les notes reçues dans une fenêtre configurable doivent être regroupées.

Valeur initiale :

```text
3 ms
```

L’allocateur doit chercher une affectation globale.

Ordre des priorités :

1. jouer le plus de notes possible ;
2. respecter les limites mécaniques ;
3. minimiser le temps avant pincement ;
4. minimiser les déplacements ;
5. conserver les doigts déjà bien positionnés ;
6. limiter les changements de direction.

## 17.3 Stratégies de saturation

Lorsque trop de notes sont demandées :

```text
ignorer les notes supplémentaires
priorité aux notes graves
priorité aux notes aiguës
priorité à la première note reçue
remplacer la note la plus ancienne
mode monophonique
```

Le choix doit être accessible dans l’interface Web.

---

# 18. Paramètres MIDI

L’interface doit permettre :

* canal MIDI global ;
* mode Omni ;
* canal par corde ;
* transposition générale ;
* transposition par corde ;
* plage de notes ;
* courbe de vélocité ;
* comportement Note Off ;
* pédale de maintien ;
* délai de regroupement des accords ;
* stratégie de saturation.

## 18.1 Vélocité

La vélocité peut agir sur :

* course du médiator ;
* vitesse du médiator ;
* délai d’attaque ;
* profil de pincement.

Courbes proposées :

```text
linéaire
douce
forte
exponentielle
personnalisée
```

---

# 19. Tableau de bord Web

La page principale doit afficher :

```text
état général
connexion Wi-Fi
source MIDI
profil actif
nombre de cordes prêtes
notes actuellement jouées
défauts actifs
températures disponibles
tensions disponibles
bouton STOP
```

Pour chaque corde :

```text
état
note actuelle
fret actuel
position moteur
position cible
distance restante
état HOME
état LIMIT
état du doigt
état du médiator
dernier défaut
```

---

# 20. Sauvegarde des configurations

Le système doit enregistrer au moins huit profils.

Fonctions :

* créer ;
* copier ;
* renommer ;
* supprimer ;
* exporter ;
* importer ;
* restaurer ;
* définir le profil de démarrage.

Le format d’échange doit être JSON.

Exemple simplifié :

```json
{
  "project": "Stepper-Plucked-Strings-GMB",
  "profileVersion": 1,
  "instrument": {
    "name": "Ukulele 4 cordes",
    "stringCount": 4,
    "pluckMode": "individual"
  },
  "board": {
    "profile": "esp32-s3-devkitc-1",
    "reserveUsb": true,
    "automaticPinAssignment": true
  },
  "network": {
    "mode": "station",
    "hostname": "gmb-ukulele"
  },
  "strings": []
}
```

Le mot de passe Wi-Fi ne doit pas apparaître dans les exports ordinaires, sauf option explicite.

---

# 21. Sécurité

## 21.1 État au démarrage

À la mise sous tension :

```text
drivers désactivés
servos neutralisés
sorties auxiliaires coupées
files MIDI vides
profil vérifié
GPIO validés
```

## 21.2 Arrêt d’urgence

Un arrêt matériel doit pouvoir :

* désactiver les drivers ;
* désactiver le PCA9685 par `OE` ;
* neutraliser les sorties auxiliaires ;
* conserver l’ESP32 alimenté.

## 21.3 Panic logiciel

Le panic doit :

* vider la file MIDI ;
* annuler tous les mouvements ;
* annuler tous les pincements ;
* relever les doigts ;
* neutraliser les servos ;
* désactiver les moteurs ;
* enregistrer la cause.

## 21.4 Perte du Wi-Fi

Comportement configurable :

```text
terminer les notes actives puis arrêter
arrêter immédiatement
continuer les commandes déjà en file
revenir en attente sans désactiver les moteurs
```

Comportement par défaut :

```text
annulation des commandes en attente
relâchement contrôlé
retour à l’état READY
```

---

# 22. Alimentation

Rails recommandés :

```text
24 V        moteurs pas à pas
5 à 7,4 V  servomoteurs
5 V         logique
3,3 V       ESP32-S3
```

Exigences :

* alimentation servo séparée ;
* fusible moteurs ;
* fusible servos ;
* protection contre l’inversion ;
* TVS sur le rail moteur ;
* condensateurs près des drivers ;
* condensateur de réserve près du PCA9685 ;
* masse commune structurée ;
* connecteurs verrouillables ;
* aucun servo alimenté par le régulateur de l’ESP32.

---

# 23. Architecture logicielle

```text
firmware/
├── application/
│   ├── Application
│   ├── Scheduler
│   └── EventBus
├── board/
│   ├── BoardProfile
│   ├── PinManager
│   └── PinValidator
├── communication/
│   ├── WifiManager
│   ├── MidiTransport
│   ├── WebSocketMidi
│   └── FutureTransports
├── midi/
│   ├── MidiParser
│   ├── MidiRouter
│   └── MidiEventQueue
├── instrument/
│   ├── InstrumentController
│   ├── StringController
│   ├── NoteAllocator
│   └── SharedStrummer
├── motion/
│   ├── StepperAxis
│   ├── MotionPlanner
│   └── HomingController
├── actuators/
│   ├── ServoManager
│   ├── FingerActuator
│   ├── PluckActuator
│   └── DamperActuator
├── configuration/
│   ├── Profile
│   ├── ProfileValidator
│   └── ProfileStorage
├── safety/
│   ├── SafetyManager
│   └── FaultManager
├── diagnostics/
│   ├── Logger
│   └── DiagnosticService
└── web/
    ├── WebServer
    ├── RestApi
    └── WebSocketStatus
```

---

# 24. Phases de développement

## Phase 1 — Prototype une corde

* ESP32-S3 ;
* Wi-Fi ;
* interface Web minimale ;
* un moteur pas à pas ;
* un capteur HOME ;
* un servo de doigt ;
* un servo de pincement ;
* test MIDI Wi-Fi ;
* machine d’état complète ;
* panic.

## Phase 2 — Configuration intuitive

* assistant de configuration ;
* profil de carte ;
* attribution automatique des GPIO ;
* validation des conflits ;
* calibration du moteur ;
* calibration des servos ;
* import/export JSON.

## Phase 3 — Multicorde

* quatre puis six axes ;
* PCA9685 ;
* homing parallèle ;
* allocation des notes ;
* accords ;
* diagnostics par corde.

## Phase 4 — Jeu avancé

* grattage partagé ;
* tremolo ;
* étouffement ;
* pédale de maintien ;
* courbes de vélocité ;
* stratégies de saturation.

## Phase 5 — Matériel dédié

* schéma électronique ;
* PCB ;
* protections ;
* connecteurs ;
* arrêt matériel ;
* validation électrique ;
* documentation de câblage.

## Phase 6 — Communications futures

* BLE MIDI ;
* USB MIDI ;
* MIDI DIN ;
* liaisons filaires complémentaires.

---

# 25. Critères d’acceptation

Le projet sera considéré fonctionnel lorsque :

1. une à six cordes peuvent être configurées ;
2. chaque corde utilise un moteur pas à pas et un doigt mobile unique ;
3. les GPIO peuvent être attribués automatiquement ;
4. l’interface ne propose que des GPIO compatibles avec la fonction ;
5. les conflits de broches sont bloqués ;
6. un débutant peut terminer la configuration avec l’assistant ;
7. le système fonctionne en point d’accès sans routeur ;
8. le système peut rejoindre un réseau Wi-Fi existant ;
9. les commandes MIDI sont reçues par Wi-Fi ;
10. les axes effectuent un homing fiable ;
11. les cordes à vide sont jouées sans appui du doigt ;
12. un Note Off annule une attaque en préparation ;
13. aucun pincement retardé n’est exécuté après une annulation ;
14. six axes peuvent être commandés simultanément ;
15. les profils peuvent être sauvegardés, exportés et restaurés ;
16. le panic neutralise tous les actionneurs ;
17. la perte du Wi-Fi produit un arrêt contrôlé ;
18. l’architecture permet l’ajout futur de BLE MIDI et de MIDI filaire.

---

# 26. Livrables

Le projet doit fournir :

```text
firmware ESP32-S3
interface Web
format des profils
profils de cartes ESP32
gestionnaire d’attribution des GPIO
schéma électronique
PCB
nomenclature
documentation de câblage
guide de première configuration
procédure de calibration
procédure de test
documentation du protocole MIDI Wi-Fi
tests automatisés
profils d’instruments d’exemple
```

---

# 27. Organisation recommandée du dépôt

```text
Stepper-Plucked-Strings-GMB/
├── firmware/
├── web-interface/
├── hardware/
│   ├── schematics/
│   ├── pcb/
│   └── wiring/
├── board-profiles/
├── instrument-profiles/
├── mechanics/
├── tests/
├── docs/
│   ├── CAHIER_DES_CHARGES.md
│   ├── ARCHITECTURE.md
│   ├── PIN_CONFIGURATION.md
│   ├── WEB_INTERFACE.md
│   ├── MIDI_PROTOCOL.md
│   ├── CALIBRATION.md
│   └── SAFETY.md
└── README.md
```

---

# 28. Décisions initiales retenues

```text
Nom : Stepper-Plucked-Strings-GMB

Projet développé à partir de zéro

Instruments à cordes pincées ou grattées uniquement

1 à 6 cordes

1 moteur pas à pas par corde

1 doigt mobile unique par corde

ESP32-S3

TMC2209 ou driver STEP/DIR compatible

PCA9685 pour les servos

Wi-Fi en première version

Interface Web locale obligatoire

Configuration accessible aux débutants

Attribution automatique ou manuelle des GPIO

Profils de cartes avec filtrage des broches

GPIO19 et GPIO20 réservés pour le futur USB

BLE MIDI et communications filaires ajoutés ultérieurement
```
