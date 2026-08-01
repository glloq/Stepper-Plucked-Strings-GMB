# Communication automatique des capacités par SysEx

## 1. Objectif

Stepper-Plucked-Strings-GMB doit communiquer automatiquement ses capacités à General-Midi-Boop.

Les informations annoncées doivent être générées depuis le profil actif enregistré dans l’ESP32.

Elles ne doivent pas être codées en dur dans le firmware.

Le système doit communiquer notamment :

* l’identité de l’appareil ;
* le nom de l’instrument ;
* la version du firmware ;
* le canal MIDI ;
* le type d’instrument ;
* le programme General MIDI ;
* le nombre de cordes ;
* l’accordage ;
* le nombre de frettes ;
* la plage de notes réellement jouable ;
* la polyphonie ;
* les contrôleurs MIDI supportés ;
* les CC utilisés pour sélectionner la corde et la frette ;
* la présence du mode de sélection explicite ;
* la révision de la configuration.

---

# 2. Principe général

```text
Configuration depuis l’interface Web
                │
                ▼
Validation du profil
                │
                ▼
Sauvegarde atomique
                │
                ▼
Génération des capacités
                │
                ▼
Publication par SysEx
                │
                ▼
General-Midi-Boop actualise l’instrument
```

Le profil actif doit être l’unique source de vérité.

Les données SysEx doivent être reconstruites automatiquement après :

* le démarrage ;
* le chargement d’un profil ;
* la modification de l’accordage ;
* la modification du nombre de cordes ;
* la modification du nombre de frettes ;
* la modification du canal MIDI ;
* la modification des CC corde/frette ;
* la modification de la polyphonie ;
* la modification du type ou du nom de l’instrument.

---

# 3. Protocole General-Midi-Boop utilisé

Tous les messages doivent utiliser l’en-tête :

```text
F0 7D 00 <bloc> <direction> ... F7
```

| Octet         | Fonction                                |
| ------------- | --------------------------------------- |
| `F0`          | début SysEx                             |
| `7D`          | identifiant SysEx expérimental/éducatif |
| `00`          | identifiant General-Midi-Boop           |
| `<bloc>`      | type d’information                      |
| `<direction>` | requête, réponse ou notification        |
| `F7`          | fin SysEx                               |

Valeurs de direction :

```text
00 = requête
01 = réponse
02 = notification spontanée proposée
```

La valeur `02` constitue une extension à ajouter conjointement dans General-Midi-Boop.

---

# 4. Blocs à implémenter

## 4.1 Bloc 1 — Identité

Le bloc 1 est obligatoire.

### Requête

```text
F0 7D 00 01 00 F7
```

### Réponse

```text
F0 7D 00 01 01
<version>
<device_id[5]>
<device_name[32]>
<firmware[3]>
<features[5]>
F7
```

### Informations annoncées

* identifiant permanent de l’appareil ;
* nom défini dans l’interface Web ;
* version majeure, mineure et corrective du firmware ;
* blocs SysEx supportés.

### Drapeaux de fonctions

Pour un instrument unique supportant les capacités et la configuration des cordes :

```text
INSTRUMENT_CAPABILITIES = 0x10
STRING_CONFIG           = 0x20

Features                = 0x30
```

Si le bloc 5 est également implémenté :

```text
INSTRUMENT_DESCRIPTOR   = 0x08

Features                = 0x38
```

### Identifiant de l’appareil

L’identifiant doit être stable après redémarrage.

Il peut être généré lors du premier démarrage à partir :

* de l’identifiant matériel de l’ESP32 ;
* d’une valeur aléatoire sauvegardée ;
* ou d’une combinaison des deux.

L’utilisateur ne doit normalement pas avoir à le modifier.

Un bouton avancé pourra permettre de régénérer cet identifiant.

---

## 4.2 Bloc 5 — Description de l’instrument

Le bloc 5 est facultatif pour la première version, car un ESP32 contrôle un seul instrument logique.

Il est néanmoins recommandé pour rendre le protocole explicite et permettre de futures extensions.

### Requête

```text
F0 7D 00 05 00 F7
```

### Réponse pour un instrument unique

```text
F0 7D 00 05 01
01
01
<canal>
<programme_gm>
<type_id>
F7
```

Exemple pour une guitare sur le canal MIDI 1 interne, correspondant au canal utilisateur 2 :

```text
F0 7D 00 05 01 01 01 01 18 04 F7
```

Avec :

```text
canal MIDI interne = 1
programme GM       = 24, guitare nylon
type               = 0x04, guitare
```

---

## 4.3 Bloc 6 — Capacités de l’instrument

Le bloc 6 est obligatoire.

### Requête

```text
F0 7D 00 06 00 <canal> F7
```

### Réponse

```text
F0 7D 00 06 01
<version>
<canal>
<programme_gm>
<type_id>
<sous_type>
<mode_notes>
<note_min>
<note_max>
<polyphonie>
<nombre_notes_discretes>
<notes_discretes...>
<nombre_cc>
<cc_supportes...>
<longueur_nom>
<nom...>
F7
```

---

# 5. Calcul automatique de la plage jouable

La plage de notes ne doit pas être saisie manuellement lorsque les cordes et les frettes sont déjà configurées.

Pour chaque corde :

```text
note minimale = note à vide + capo

note maximale =
note à vide
+ capo
+ nombre maximal de frettes de la corde
```

Le contrôleur construit ensuite l’union de toutes les notes jouables.

## 5.1 Mode plage continue

Le mode plage peut être utilisé lorsque toutes les notes comprises entre la note minimale et la note maximale sont jouables.

```text
mode_notes = 0
```

Le bloc contient alors :

```text
note_min
note_max
nombre_notes_discretes = 0
```

## 5.2 Mode notes discrètes

Si certaines notes situées entre les limites ne sont jouables sur aucune corde :

```text
mode_notes = 1
```

Le système doit transmettre la liste exacte des notes jouables.

Ce cas peut apparaître avec :

* des accordages particuliers ;
* peu de frettes ;
* des nombres de frettes différents selon les cordes ;
* des cordes désactivées ;
* des zones mécaniques interdites.

---

# 6. Calcul de la polyphonie

La polyphonie annoncée doit représenter le nombre maximal de notes pouvant être maintenues simultanément.

Valeur automatique initiale :

```text
polyphonie =
nombre de cordes actives et fonctionnelles
```

Elle doit cependant pouvoir être limitée dans l’interface Web.

Exemples :

```text
6 cordes avec médiators individuels → polyphonie maximale 6

6 cordes avec un grattage partagé
mais maintien indépendant           → polyphonie maximale 6

6 cordes avec contraintes mécaniques
limitant le jeu à 4 cordes           → polyphonie configurée à 4
```

Le réglage doit proposer :

```text
Automatique
Valeur personnalisée
```

---

# 7. Contrôleurs MIDI annoncés

Le bloc 6 doit déclarer les CC réellement activés.

Liste possible :

|    CC | Fonction                       |
| ----: | ------------------------------ |
|   CC7 | volume général                 |
|  CC11 | expression                     |
|  CC20 | sélection de corde par défaut  |
|  CC21 | sélection de frette par défaut |
|  CC64 | maintien                       |
| CC120 | arrêt sonore immédiat          |
| CC123 | arrêt de toutes les notes      |

Les CC20 et CC21 doivent être remplacés par les numéros réellement configurés dans l’interface.

Exemple :

```text
CC corde configuré = 24
CC frette configuré = 25
```

Le bloc 6 doit annoncer :

```text
7, 11, 24, 25, 64, 120, 123
```

Un CC désactivé ne doit pas être annoncé.

---

# 8. Bloc 7 — Configuration des cordes

Le bloc 7 est obligatoire.

### Requête

```text
F0 7D 00 07 00 <canal> F7
```

### Réponse compatible General-Midi-Boop version 1

```text
F0 7D 00 07 01
01
<canal>
<nombre_cordes>
<nombre_frettes>
<is_fretless>
<capo>
<cc_active>
<cc_corde>
<cc_frette>
<accordage...>
F7
```

Pour Stepper-Plucked-Strings-GMB :

```text
is_fretless = 0
```

L’accordage doit être transmis dans l’ordre défini par General-Midi-Boop :

```text
corde la plus grave
vers
corde la plus aiguë
```

---

# 9. Limites du bloc 7 version 1

La version actuelle du bloc 7 transmet :

* un nombre global de frettes ;
* les numéros des CC corde et frette ;
* l’accordage.

Elle ne transmet pas encore :

* un nombre de frettes différent par corde ;
* les valeurs minimale et maximale des CC ;
* les offsets des CC ;
* l’ordre inversé des cordes ;
* la table personnalisée corde logique vers corde physique ;
* le mode automatique, explicite ou hybride.

Ces réglages restent enregistrés dans l’ESP32, mais ne peuvent pas tous être transmis avec le bloc 7 version 1.

La première version du firmware doit rester compatible avec le bloc 7 version 1.

Une extension version 2 devra être ajoutée conjointement dans General-Midi-Boop.

---

# 10. Extension proposée du bloc 7 version 2

## 10.1 Objectif

La version 2 doit transmettre toute la configuration nécessaire au système de tablature et de sélection corde/frette.

### Réponse proposée

```text
F0 7D 00 07 01
02
<canal>
<nombre_cordes>
<nombre_frettes_global>
<is_fretless>
<capo>

<cc_active>
<cc_corde>
<cc_frette>

<cc_corde_min>
<cc_corde_max>
<cc_corde_offset_encode>

<cc_frette_min>
<cc_frette_max>
<cc_frette_offset_encode>

<mode_selection>
<ordre_cordes>

<accordage[num_cordes]>
<frettes_par_corde[num_cordes]>
<mapping_cordes[num_cordes]>
F7
```

## 10.2 Modes de sélection

```text
0 = allocation automatique
1 = sélection explicite par CC
2 = mode hybride
```

## 10.3 Ordre des cordes

```text
0 = ordre normal
1 = ordre inversé
2 = correspondance personnalisée
```

## 10.4 Offsets signés

Comme les données SysEx doivent rester comprises entre 0 et 127, un offset signé doit être encodé ainsi :

```text
valeur_transmise = offset + 64
```

Plage disponible :

```text
-64 à +63
```

Décodage :

```text
offset = valeur_transmise - 64
```

## 10.5 Compatibilité

Le firmware doit pouvoir répondre en version 1 tant que General-Midi-Boop ne prend pas en charge la version 2.

La prise en charge de la version 2 doit être activée uniquement après ajout de son parseur dans General-Midi-Boop.

---

# 11. Notification de modification des capacités

## 11.1 Problème

Le protocole actuel est initié par General-Midi-Boop :

```text
General-Midi-Boop envoie une requête
ESP32 envoie une réponse
```

Si l’utilisateur modifie la configuration depuis l’interface Web de l’ESP32, General-Midi-Boop ne sait pas automatiquement que les capacités ont changé.

## 11.2 Solution proposée

Ajouter un bloc de notification :

```text
Bloc 8 — Capabilities Changed
```

La notification ne transporte pas toutes les capacités.

Elle indique à General-Midi-Boop qu’il doit relancer la procédure de découverte.

### Format proposé

```text
F0 7D 00 08 02
01
<canal>
<revision_configuration[5]>
<flags_modification>
F7
```

### Champs

| Champ    | Description                                        |
| -------- | -------------------------------------------------- |
| `08`     | bloc notification                                  |
| `02`     | notification spontanée                             |
| `01`     | version du bloc                                    |
| canal    | canal concerné                                     |
| révision | numéro de configuration encodé sur 5 octets 7 bits |
| flags    | catégories modifiées                               |

### Drapeaux de modification

| Bit | Signification                        |
| --: | ------------------------------------ |
|   0 | identité modifiée                    |
|   1 | descripteur modifié                  |
|   2 | capacités générales modifiées        |
|   3 | configuration des cordes modifiée    |
|   4 | mapping CC modifié                   |
|   5 | redémarrage ou nouveau homing requis |
|   6 | réservé                              |

---

# 12. Réaction attendue de General-Midi-Boop

Après réception du bloc 8 :

```text
1. identifier le périphérique ;
2. lire le canal concerné ;
3. comparer le numéro de révision ;
4. envoyer une nouvelle requête Bloc 1 ;
5. relire les drapeaux de fonctions ;
6. envoyer une requête Bloc 6 ;
7. envoyer une requête Bloc 7 ;
8. envoyer une requête Bloc 5 si nécessaire ;
9. actualiser l’instrument ;
10. signaler la nouvelle configuration dans l’interface.
```

General-Midi-Boop ne doit pas remplacer la configuration tant que toutes les réponses attendues ne sont pas valides.

En cas d’échec, l’ancienne configuration doit rester active avec un avertissement.

---

# 13. Numéro de révision de configuration

Chaque sauvegarde valide doit incrémenter un compteur :

```text
capabilitiesRevision
```

Exemple :

```text
configuration initiale : 1
modification accordage : 2
modification CC        : 3
modification frettes   : 4
```

Le compteur doit être :

* sauvegardé dans la mémoire persistante ;
* inclus dans la notification du bloc 8 ;
* affiché dans l’interface Web ;
* utilisé pour éviter les actualisations inutiles.

Un simple redémarrage sans modification ne doit pas nécessairement incrémenter la révision.

---

# 14. Fonctionnement au démarrage

```text
Démarrage ESP32
      │
      ▼
Chargement du profil actif
      │
      ▼
Validation
      │
      ├── profil invalide → mode SAFE
      │
      ▼
Construction du snapshot de capacités
      │
      ▼
Initialisation du transport MIDI Wi-Fi
      │
      ▼
Attente des requêtes SysEx
```

Lorsque General-Midi-Boop se connecte :

```text
Bloc 1 demandé
      ↓
réponse identité
      ↓
Bloc 6 demandé
      ↓
réponse capacités
      ↓
Bloc 7 demandé
      ↓
réponse configuration des cordes
```

---

# 15. Fonctionnement après modification Web

```text
Utilisateur modifie un paramètre
              │
              ▼
Configuration placée en brouillon
              │
              ▼
Validation complète
              │
      ┌───────┴────────┐
      │                │
   invalide          valide
      │                │
afficher erreur         ▼
               sauvegarde atomique
                        │
                        ▼
               incrément de révision
                        │
                        ▼
               reconstruction capacités
                        │
                        ▼
                 notification Bloc 8
```

Une configuration en cours d’édition ne doit jamais être annoncée.

Seul le profil validé et activé doit être publié.

---

# 16. Application des modifications mécaniques

Certaines modifications peuvent être appliquées immédiatement :

* nom ;
* programme GM ;
* numéros de CC ;
* mode de sélection ;
* stratégie d’allocation.

D’autres nécessitent une remise en sécurité :

* changement du nombre de cordes ;
* changement de broches ;
* changement du rapport pas/mm ;
* changement du sens moteur ;
* changement des limites ;
* changement du capteur HOME ;
* changement des positions de frettes.

Pour ces modifications :

```text
1. arrêter les nouvelles notes ;
2. terminer ou annuler les notes actives ;
3. désactiver les actionneurs ;
4. appliquer la configuration ;
5. effectuer un homing si nécessaire ;
6. reconstruire les capacités ;
7. envoyer la notification SysEx.
```

L’interface doit indiquer clairement :

```text
Application immédiate
Homing nécessaire
Redémarrage nécessaire
```

---

# 17. Interface Web — Identité et capacités

Une page doit être ajoutée :

```text
MIDI > Identité et capacités GMB
```

## 17.1 Mode simplifié

Le mode simplifié doit proposer :

* activation de la détection General-Midi-Boop ;
* nom de l’instrument ;
* type d’instrument ;
* préréglage d’instrument ;
* programme General MIDI ;
* canal MIDI ;
* bouton « Publier les capacités » ;
* bouton « Tester la communication » ;
* état de la dernière détection.

Les capacités calculées doivent être affichées en lecture seule :

```text
Cordes              : 4
Frettes              : 12
Plage MIDI           : 40 à 76
Polyphonie           : 4
CC corde             : 20
CC frette            : 21
Accordage             : E2 A2 D3 G3
Révision             : 7
```

## 17.2 Mode avancé

Le mode avancé doit permettre :

* activation des blocs 5, 6 et 7 ;
* sélection de la version du bloc 7 ;
* surcharge de la polyphonie calculée ;
* choix plage continue ou notes discrètes ;
* visualisation des CC annoncés ;
* visualisation des octets SysEx ;
* envoi manuel de chaque réponse ;
* envoi de la notification de modification ;
* réinitialisation de l’identifiant ;
* export du snapshot des capacités.

---

# 18. Testeur SysEx intégré

L’interface doit pouvoir simuler les requêtes suivantes :

```text
Demander identité
Demander descripteur
Demander capacités
Demander configuration des cordes
Notifier une modification
Effectuer une découverte complète
```

Pour chaque test, l’interface doit afficher :

* message envoyé ;
* message reçu ;
* décodage des champs ;
* validité 7 bits ;
* longueur ;
* éventuelle erreur ;
* durée de réponse.

Exemple :

```text
Requête :
F0 7D 00 06 00 00 F7

Réponse :
F0 7D 00 06 01 ...

Canal       : 1
Type        : guitare
Plage       : E2 à E5
Polyphonie  : 4
CC          : 7, 11, 20, 21, 64, 120, 123
Résultat    : valide
```

---

# 19. Snapshot des capacités

Le firmware doit produire une structure interne immuable :

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
```

Lorsqu’une réponse SysEx commence, elle doit utiliser un seul snapshot.

Une modification de configuration pendant l’envoi ne doit pas produire une réponse mélangeant deux versions du profil.

---

# 20. Sécurité du protocole

Le firmware doit :

* vérifier chaque longueur de requête ;
* vérifier l’en-tête ;
* vérifier l’identifiant General-Midi-Boop ;
* ignorer les directions inconnues ;
* limiter tous les octets de données entre 0 et 127 ;
* limiter la taille maximale d’un message ;
* éviter toute allocation mémoire dynamique pendant le traitement ;
* limiter la fréquence des réponses ;
* refuser les canaux inexistants ;
* ne jamais transmettre de mot de passe Wi-Fi ;
* ne jamais transmettre de données sensibles.

Les messages SysEx inconnus doivent être ignorés sans affecter le fonctionnement musical.

---

# 21. Indépendance du transport

Le gestionnaire SysEx doit être indépendant du transport MIDI.

```text
Wi-Fi MIDI ───┐
BLE MIDI ─────┤
USB MIDI ─────┼──► MidiMessageRouter ─► GmbSysExService
MIDI DIN ─────┤
série ────────┘
```

La première version utilise le Wi-Fi.

Les futures versions Bluetooth et filaires doivent réutiliser exactement :

* les mêmes blocs ;
* le même encodeur ;
* le même décodeur ;
* le même snapshot ;
* les mêmes tests.

Le transport doit seulement transmettre des octets MIDI complets.

---

# 22. Compatibilité initiale

## Première version

La première version doit implémenter :

```text
Bloc 1 version 1
Bloc 5 version 1, recommandé
Bloc 6 version 1
Bloc 7 version 1
```

Cela assure la compatibilité avec le protocole actuel de General-Midi-Boop.

## Extension suivante

Une évolution coordonnée des deux dépôts doit ajouter :

```text
Bloc 7 version 2
Bloc 8 notification de modification
```

Tant que le bloc 8 n’est pas supporté par General-Midi-Boop, l’interface doit fournir :

```text
Réannoncer les capacités
```

Cette action pourra :

* envoyer les réponses SysEx de manière spontanée ;
* ou provoquer une reconnexion du transport ;
* ou demander à General-Midi-Boop de relancer sa détection.

La méthode exacte devra être alignée avec l’implémentation de General-Midi-Boop.

---

# 23. Tests obligatoires

## Identification

* réponse correcte au bloc 1 ;
* identifiant stable ;
* nom correctement tronqué ;
* version correcte ;
* drapeaux corrects.

## Capacités

* plage calculée correctement ;
* notes discrètes correctement générées ;
* polyphonie correcte ;
* CC configurés correctement annoncés ;
* nom et type cohérents.

## Cordes

* nombre de cordes correct ;
* nombre de frettes correct ;
* accordage dans le bon ordre ;
* capo correct ;
* CC20/CC21 par défaut ;
* CC personnalisés correctement annoncés.

## Modifications

* changement de nom ;
* changement d’accordage ;
* changement de CC ;
* changement de nombre de frettes ;
* changement de nombre de cordes ;
* incrément de révision ;
* notification envoyée ;
* nouvelle interrogation réussie.

## Robustesse

* requête tronquée ;
* canal invalide ;
* bloc inconnu ;
* données supérieures à 127 ;
* requêtes répétées rapidement ;
* modification pendant une interrogation ;
* perte du Wi-Fi pendant une réponse.

---

# 24. Critères d’acceptation

La communication des capacités sera considérée fonctionnelle lorsque :

1. l’ESP32 répond à la requête d’identité ;
2. son identifiant reste stable ;
3. les fonctionnalités annoncées correspondent aux blocs disponibles ;
4. General-Midi-Boop peut lire automatiquement le type d’instrument ;
5. la plage de notes est calculée depuis l’accordage et les frettes ;
6. la polyphonie est calculée ou configurée ;
7. les CC supportés sont annoncés ;
8. le nombre de cordes et l’accordage sont transmis ;
9. les CC de corde et de frette sont transmis ;
10. les réponses sont générées depuis le profil actif ;
11. une configuration en brouillon n’est jamais publiée ;
12. toute modification valide incrémente la révision ;
13. une modification peut déclencher une nouvelle découverte ;
14. le firmware reste compatible avec les blocs 1, 6 et 7 version 1 ;
15. l’architecture permet l’ajout du bloc 7 version 2 ;
16. l’architecture permet l’ajout de la notification bloc 8 ;
17. le service SysEx fonctionne indépendamment du transport MIDI.
