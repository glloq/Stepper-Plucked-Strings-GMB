# Sélection explicite de la corde et de la frette par MIDI CC

## 1. Objectif

Stepper-Plucked-Strings-GMB doit pouvoir recevoir une indication explicite de la corde et de la frette à utiliser avant le déclenchement d’une note.

Cette fonction permet au système de contrôle principal, notamment General-Midi-Boop, de transmettre directement une position de tablature :

```text
sélection de la corde
        ↓
sélection de la frette
        ↓
Note On
```

Convention par défaut :

```text
CC20 = numéro de corde
CC21 = numéro de frette
```

Exemple :

```text
CC20 valeur 3
CC21 valeur 5
Note On 60 vélocité 100
```

Interprétation :

```text
jouer la note MIDI 60
sur la corde physique 3
à la frette 5
avec une vélocité de 100
```

Les numéros de contrôleurs et les valeurs ne doivent pas être codés en dur. Tous les paramètres doivent être modifiables depuis l’interface Web.

---

## 2. Modes de sélection

L’interface doit proposer trois modes.

### 2.1 Allocation automatique

```text
Mode : Automatique
```

Le contrôleur reçoit uniquement le `Note On`.

Il choisit lui-même :

* la corde capable de jouer la note ;
* la frette correspondante ;
* la corde demandant le moins de déplacement ;
* la combinaison la plus adaptée pour un accord.

Ce mode reste compatible avec les fichiers MIDI standards ne contenant aucun CC de tablature.

### 2.2 Sélection explicite par CC

```text
Mode : Corde et frette imposées par MIDI CC
```

Le contrôleur utilise les CC reçus avant le `Note On`.

La corde et la frette sont imposées par l’émetteur MIDI.

Ce mode est destiné en priorité à General-Midi-Boop et aux fichiers MIDI contenant des informations de tablature.

### 2.3 Mode hybride

```text
Mode : CC prioritaire avec allocation automatique de secours
```

Comportement :

1. utiliser la sélection CC lorsqu’elle est complète et valide ;
2. vérifier que la corde et la frette peuvent jouer la note ;
3. utiliser l’allocation automatique si aucune sélection valide n’est disponible.

Ce mode doit être sélectionné par défaut, car il permet de lire :

* des fichiers MIDI standards ;
* des fichiers MIDI enrichis ;
* des commandes provenant de General-Midi-Boop ;
* des notes jouées depuis l’interface Web.

---

## 3. Préréglage General-Midi-Boop

L’interface doit proposer un bouton :

```text
Utiliser le préréglage General-Midi-Boop
```

Ce bouton applique automatiquement :

| Paramètre                        |         Valeur |
| -------------------------------- | -------------: |
| Sélection explicite activée      |            oui |
| CC de sélection de corde         |             20 |
| CC de sélection de frette        |             21 |
| Première corde                   |       valeur 1 |
| Première frette                  |       valeur 0 |
| Offset corde                     |              0 |
| Offset frette                    |              0 |
| Mode de consommation             | prochaine note |
| Sélection par canal MIDI         |            oui |
| Allocation de secours            |    automatique |
| Préparation dès réception des CC |            oui |

Les valeurs maximales doivent être adaptées automatiquement au profil actif :

```text
CC corde minimum = 1
CC corde maximum = nombre de cordes

CC frette minimum = 0
CC frette maximum = frette maximale de l’instrument
```

Pour un instrument à quatre cordes et douze frettes :

```text
CC20 : valeurs valides 1 à 4
CC21 : valeurs valides 0 à 12
```

---

## 4. Paramètres configurables depuis l’interface Web

La page MIDI doit contenir une section :

```text
Sélection de corde et de frette
```

### 4.1 Réglages généraux

* activation de la sélection explicite ;
* mode automatique, explicite ou hybride ;
* canal MIDI global ;
* sélection indépendante par canal MIDI ;
* délai maximal entre les CC et le `Note On` ;
* comportement en cas de sélection incomplète ;
* comportement en cas de valeur invalide ;
* mode de validation entre la note, la corde et la frette.

### 4.2 Réglages de sélection de corde

```text
Contrôleur MIDI utilisé
Valeur minimale reçue
Valeur maximale reçue
Offset
Numérotation à partir de 0 ou de 1
Ordre normal ou inversé
Table de correspondance personnalisée
```

Valeurs par défaut :

```text
CC             = 20
minimum        = 1
maximum        = nombre de cordes
offset         = 0
numérotation   = à partir de 1
ordre          = normal
```

### 4.3 Réglages de sélection de frette

```text
Contrôleur MIDI utilisé
Valeur minimale reçue
Valeur maximale reçue
Offset
Arrondi des valeurs
Gestion des frettes hors plage
Table de correspondance personnalisée
```

Valeurs par défaut :

```text
CC             = 21
minimum        = 0
maximum        = frette maximale
offset         = 0
```

### 4.4 Délai de validité

La sélection ne doit pas rester active indéfiniment.

Paramètre :

```text
Durée de validité de la sélection
```

Valeur proposée :

```text
100 ms par défaut
plage réglable : 5 à 2000 ms
```

Si aucun `Note On` correspondant n’est reçu pendant ce délai, la sélection doit être supprimée.

---

## 5. Transformation des valeurs reçues

### 5.1 Corde

Calcul général :

```text
corde logique =
valeur CC reçue
+ offset
```

La valeur doit ensuite être :

* validée ;
* limitée à la plage autorisée ;
* convertie vers l’indice interne ;
* associée à un axe pas-à-pas.

Exemple avec une numérotation à partir de 1 :

```text
CC20 = 1 → corde interne 0
CC20 = 2 → corde interne 1
CC20 = 3 → corde interne 2
CC20 = 4 → corde interne 3
```

### 5.2 Frette

Calcul général :

```text
frette logique =
valeur CC reçue
+ offset
```

Exemple :

```text
CC21 = 0 → corde à vide
CC21 = 1 → première frette
CC21 = 12 → douzième frette
```

La frette zéro doit automatiquement entraîner :

```text
doigt relevé
aucun appui sur la corde
pincement de la corde à vide
```

---

## 6. Inversion de l’ordre des cordes

L’ordre physique des cordes peut différer de l’ordre utilisé dans une tablature ou dans General-Midi-Boop.

L’interface doit proposer :

```text
Ordre normal
Ordre inversé
Correspondance personnalisée
```

Exemple pour un instrument à quatre cordes :

### Ordre normal

| Valeur CC | Corde physique |
| --------: | -------------: |
|         1 |              1 |
|         2 |              2 |
|         3 |              3 |
|         4 |              4 |

### Ordre inversé

| Valeur CC | Corde physique |
| --------: | -------------: |
|         1 |              4 |
|         2 |              3 |
|         3 |              2 |
|         4 |              1 |

### Correspondance personnalisée

L’utilisateur doit pouvoir sélectionner manuellement :

```text
Valeur CC 1 → axe 3
Valeur CC 2 → axe 1
Valeur CC 3 → axe 4
Valeur CC 4 → axe 2
```

Un schéma visuel doit afficher la correspondance entre :

* numéro MIDI ;
* corde musicale ;
* corde physique ;
* axe pas-à-pas ;
* note à vide.

---

## 7. Machine de réception MIDI

Le contrôleur doit stocker les sélections sous forme de commandes temporaires.

```cpp
struct PendingStringSelection {
    uint8_t midiChannel;

    bool hasString;
    bool hasFret;

    uint8_t stringValue;
    uint8_t fretValue;

    uint32_t receivedAtUs;
    uint32_t expiresAtUs;
};
```

Une sélection complète contient :

```text
canal MIDI
corde
frette
horodatage
état de validation
```

---

## 8. Gestion fiable des accords

Plusieurs notes simultanées peuvent générer plusieurs sélections corde/frette sur le même canal MIDI.

Le firmware ne doit donc pas conserver uniquement :

```text
dernière corde reçue
dernière frette reçue
```

Cette méthode ne serait pas fiable pour les accords.

Le système doit utiliser une file FIFO de sélections.

Exemple d’événements reçus :

```text
CC20 corde 1
CC20 corde 3
CC20 corde 4
CC21 frette 2
CC21 frette 5
CC21 frette 7
Note On 42
Note On 55
Note On 64
```

Le système doit reconstruire :

```text
sélection 1 = corde 1, frette 2
sélection 2 = corde 3, frette 5
sélection 3 = corde 4, frette 7
```

Puis associer les `Note On` dans le même ordre :

```text
Note 42 → sélection 1
Note 55 → sélection 2
Note 64 → sélection 3
```

La file doit pouvoir contenir au minimum :

```text
16 sélections en attente
```

Valeur recommandée :

```text
32 sélections
```

---

## 9. Algorithme d’association

### Réception du CC de corde

```text
1. lire la valeur ;
2. appliquer l’offset ;
3. convertir vers la corde physique ;
4. vérifier la plage ;
5. créer une nouvelle sélection en attente ;
6. enregistrer la corde.
```

### Réception du CC de frette

```text
1. lire la valeur ;
2. appliquer l’offset ;
3. vérifier la plage ;
4. rechercher la plus ancienne sélection sans frette ;
5. ajouter la frette à cette sélection ;
6. marquer la sélection comme complète.
```

### Réception du Note On

```text
1. rechercher la plus ancienne sélection complète du canal ;
2. associer la note à cette sélection ;
3. valider la cohérence note/corde/frette ;
4. retirer la sélection de la file ;
5. préparer le moteur ;
6. programmer l’appui et le pincement.
```

Cette méthode reste fonctionnelle si les événements d’un accord sont regroupés par type.

---

## 10. Préparation anticipée

Dès qu’une paire corde/frette complète est reçue, le contrôleur doit pouvoir commencer la préparation mécanique, sans attendre le `Note On`.

Séquence :

```text
CC corde reçu
        ↓
CC frette reçu
        ↓
sélection complète
        ↓
relâchement du doigt
        ↓
déplacement du moteur
        ↓
Note On reçu
        ↓
appui et pincement lorsque la position est prête
```

Ce comportement doit être configurable :

```text
Préparer le moteur dès réception de la sélection
```

Valeur par défaut :

```text
activé
```

Le `Note On` conserve son rôle de déclenchement musical.

Si le moteur n’a pas encore atteint la frette au moment du `Note On` :

* le pincement doit être mis en attente ;
* le moteur doit terminer son déplacement ;
* le doigt doit être appuyé ;
* le pincement doit ensuite être exécuté ;
* aucun pincement anticipé ne doit être produit.

---

## 11. Cohérence entre note, corde et frette

Pour une corde frettée standard :

```text
note attendue =
note à vide
+ numéro de frette
+ transposition éventuelle
```

Le contrôleur doit comparer :

* la note MIDI reçue ;
* la note à vide de la corde ;
* la frette sélectionnée ;
* le capo ;
* la transposition configurée.

L’interface doit proposer trois comportements.

### Priorité aux CC

```text
La corde et la frette sont utilisées.
La différence avec le Note On est seulement signalée.
```

### Priorité à la note

```text
La frette est recalculée à partir de la note MIDI.
```

### Mode strict

```text
La note est refusée si les informations sont incohérentes.
```

Valeur par défaut recommandée :

```text
Priorité aux CC avec avertissement
```

General-Midi-Boop doit rester capable d’imposer une tablature précise.

---

## 12. Gestion du Note Off

Le contrôleur doit mémoriser l’affectation réelle de chaque `Note On`.

```cpp
struct ActiveNote {
    uint8_t midiChannel;
    uint8_t midiNote;
    uint8_t stringIndex;
    uint8_t fret;
    uint32_t noteInstanceId;
};
```

Le `Note Off` ne doit pas utiliser la dernière valeur de CC reçue.

Il doit retrouver l’affectation enregistrée lors du `Note On`.

Cela permet :

* de relâcher la bonne corde ;
* de gérer plusieurs cordes simultanément ;
* de gérer les accords ;
* d’éviter qu’une nouvelle sélection modifie une note déjà active.

Pour les notes répétées de même hauteur, une pile d’instances doit être utilisée.

---

## 13. Valeurs invalides

L’interface doit permettre de sélectionner le comportement suivant :

```text
Refuser la commande
Limiter à la plage autorisée
Utiliser l’allocation automatique
Utiliser la dernière valeur valide
```

Valeur par défaut :

```text
Utiliser l’allocation automatique et enregistrer un avertissement
```

Exemples de valeurs invalides :

* corde inexistante ;
* frette supérieure à la capacité de la corde ;
* axe désactivé ;
* corde en défaut ;
* frette non calibrée ;
* sélection expirée ;
* paire CC incomplète.

---

## 14. Configuration simplifiée pour débutant

L’écran simplifié doit afficher uniquement :

```text
[✓] Activer la sélection corde/frette

Système utilisé :
[ General-Midi-Boop ]

CC de corde :
[ 20 ]

CC de frette :
[ 21 ]

Numérotation des cordes :
[ 1 à 6 ]

Ordre des cordes :
[ Normal ]

En cas de CC absent :
[ Choisir automatiquement ]
```

Boutons disponibles :

```text
Appliquer le préréglage
Tester la réception
Envoyer un test
Voir les valeurs reçues
```

Les paramètres avancés doivent rester masqués dans une section :

```text
Réglages avancés
```

---

## 15. Moniteur MIDI Web

L’interface doit afficher en temps réel :

| Temps | Canal | Message    | Valeur | Interprétation    |
| ----: | ----: | ---------- | -----: | ----------------- |
|  0 ms |     1 | CC20       |      3 | corde 3           |
|  1 ms |     1 | CC21       |      5 | frette 5          |
|  2 ms |     1 | Note On 60 |    100 | corde 3, frette 5 |

Le moniteur doit également afficher :

* sélection complète ;
* sélection en attente ;
* sélection expirée ;
* valeur invalide ;
* allocation automatique utilisée ;
* incohérence entre note et frette ;
* corde physique réellement sélectionnée.

Un bouton doit permettre de vider le journal.

---

## 16. Outil de test intégré

L’interface doit proposer un panneau permettant de choisir :

```text
Corde
Frette
Note MIDI
Vélocité
Canal MIDI
```

Puis d’envoyer automatiquement :

```text
CC corde
CC frette
Note On
Note Off après une durée choisie
```

Le test doit afficher chaque étape :

```text
CC corde reçu
CC frette reçu
sélection validée
axe en déplacement
position atteinte
doigt appuyé
corde pincée
```

---

## 17. Paramètres du profil JSON

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

    "string": {
      "ccNumber": 20,
      "minimum": 1,
      "maximum": 6,
      "offset": 0,
      "numbering": "oneBased",
      "reverseOrder": false,
      "mapping": [0, 1, 2, 3, 4, 5]
    },

    "fret": {
      "ccNumber": 21,
      "minimum": 0,
      "maximum": 24,
      "offset": 0,
      "invalidValuePolicy": "automaticFallback"
    },

    "validation": {
      "notePositionPolicy": "ccPriorityWithWarning",
      "missingSelectionPolicy": "automaticAllocation",
      "expiredSelectionPolicy": "automaticAllocation"
    }
  }
}
```

---

## 18. Validation de la configuration

Le système doit vérifier :

* CC corde compris entre 0 et 119 ;
* CC frette compris entre 0 et 119 ;
* deux numéros de CC différents ;
* absence de conflit avec une autre fonction configurée ;
* plage de cordes compatible avec le nombre de cordes ;
* plage de frettes compatible avec le profil ;
* correspondance complète entre valeurs et axes ;
* profondeur de file suffisante ;
* délai de validité non nul.

Les CC120 à CC127 ne doivent pas être proposés, car ils correspondent aux messages MIDI de mode de canal.

L’interface peut autoriser les CC standards en mode avancé, mais doit afficher un avertissement en cas de conflit potentiel.

Les CC20 et CC21 doivent être présentés comme choix recommandés.

---

## 19. Critères d’acceptation

La sélection corde/frette sera considérée fonctionnelle lorsque :

1. CC20 sélectionne une corde par défaut ;
2. CC21 sélectionne une frette par défaut ;
3. les numéros de CC sont modifiables ;
4. les plages et offsets sont modifiables ;
5. l’ordre des cordes peut être inversé ;
6. une table de correspondance personnalisée peut être définie ;
7. le mode automatique reste disponible ;
8. le mode hybride utilise les CC puis une allocation de secours ;
9. les sélections sont séparées par canal MIDI ;
10. les sélections expirées sont supprimées ;
11. plusieurs sélections simultanées peuvent être mises en file ;
12. les accords ne dépendent pas uniquement de la dernière valeur reçue ;
13. le déplacement peut commencer dès réception de la paire CC ;
14. le `Note On` est associé à la bonne corde et à la bonne frette ;
15. le `Note Off` relâche la corde réellement utilisée ;
16. l’interface affiche les événements reçus et leur interprétation ;
17. le préréglage General-Midi-Boop peut être appliqué en un clic ;
18. une configuration incorrecte est détectée avant l’activation de l’instrument.
