# Généralisation du modèle d'instrument (audit P2.18)

> **But de ce document** : *identifier et documenter* les endroits du cœur qui
> supposent aujourd'hui « 6 cordes, 24 frettes, `note = corde à vide + frette` »,
> et esquisser l'abstraction future qui permettra 7/8 cordes, mandolines à
> chœurs doubles, cithares, harpes, etc.
>
> **Ce document ne demande aucune réécriture immédiate.** Le modèle actuel
> *un chariot par corde* reste la bonne implémentation tant que la
> généralisation n'est pas nécessaire. On documente les points de dépendance pour ne pas se
> bloquer plus tard, pas pour complexifier le runtime maintenant.

## 1. Hypothèses codées en dur et où elles vivent

Les trois hypothèses sont **peu nombreuses et déjà centralisées** — il n'y a pas
de dépendance « artificielle » éparpillée à retirer d'urgence. Elles sont
concentrées dans le cœur (`firmware/src/core/`) :

| Hypothèse | Constante / fonction | Fichiers dépendants | Nature |
| --------- | -------------------- | ------------------- | ------ |
| ≤ 6 cordes | `kMaxStrings = 6` (`core/Types.h`) | `ProfileValidator.cpp` (bornes `stringCount`, `polyphonyMax`), `StringFretSelector.h` (`string.maximum`) | **Borne**, facile à relever |
| ≤ 24 frettes | `kMaxFret = 24` (`core/Types.h`) | `ProfileValidator.cpp`, `StringFretSelector.h` | **Borne** |
| `note = corde à vide + frette` | `frettedNote()` (`core/Types.h`) | `NoteAllocator.cpp`, `gmb/Capabilities.cpp` | **Loi de gamme** (tempérament égal) |
| `position = f(longueur, frette)` | `fretPositionMm()` (`core/Types.h`), surchargeable par `calibratedFretMm` | `StringController.cpp`, `InstrumentController.cpp` | **Loi de géométrie** — propre à ce build : ici la frette est une *position en mm*, pas un servo |

Points importants :

* **La loi note↔position est déjà un seul point d'entrée** : `frettedNote(openNote,
  fret, capo, transpose)` (`core/Types.h`). `NoteAllocator` et `Capabilities`
  l'appellent au lieu de recalculer `open + fret` à la main. C'est le *seam*
  propre par lequel une future abstraction remplacera l'arithmétique de gamme par
  une table de positions jouables.
* **La loi note↔position mécanique est elle aussi un seul point d'entrée** :
  `fretPositionMm(scaleLengthMm, fret)` (tempérament égal), que
  `AxisConfig::calibratedFretMm` peut remplacer frette par frette. C'est le
  second *seam* : une géométrie non tempérée (cithare, harpe) se décrit déjà
  entièrement par une table de positions calibrées, sans toucher au runtime.
* Contrairement au build servo-par-frette, il n'y a **pas de masque de frettes** :
  toute position entre le sillet et la butée est atteignable par le chariot, donc
  l'ensemble des frettes jouables est un *intervalle* (`0..maxFret`) et non un
  ensemble arbitraire. C'est une contrainte en moins pour la généralisation.
* Aucune de ces dépendances n'est accidentelle : ce sont les bornes réelles du
  modèle actuel. Il n'y a donc **rien de trivial à supprimer** aujourd'hui ; la
  généralisation est une *abstraction* à introduire, documentée ci-dessous.

## 2. Abstraction future — `Voice` / `Course`

Le modèle *un chariot par corde* deviendra **une implémentation particulière**
d'une abstraction plus générale :

```text
Voice / Course
 ├─ positions jouables
 │   ├─ note MIDI                (au lieu de open + fret)
 │   └─ actionneur mécanique     (chariot+doigt / servo de frette / capo / rien)
 └─ mécanisme d'excitation       (plectre, marteau, archet…)
```

* une **Voice/Course** remplace « corde » : elle porte une *liste de positions
  jouables*, chacune associant une **note MIDI** (issue d'une table, plus
  forcément `open + fret`) à un **actionneur** — ici un couple
  *(position en mm du chariot, doigt)*, ailleurs un servo dédié, ou aucun pour
  une corde déjà à la bonne hauteur (harpe, cithare) ;
* le **mécanisme d'excitation** (pluck / strum / marteau / archet) devient un
  attribut de la Voice, pas une supposition « un plectre par corde » ;
* un chœur double (mandoline) est une Voice pilotant deux cordes physiques
  d'une même note ; une harpe est une Voice à une seule position (corde à vide) ;
  une cithare, plusieurs Voices sans doigt.

## 3. Ce qui débloque, à terme

* 7 / 8 cordes (relever `kMaxStrings` + élargir le masque de frettes) ;
* mandolines à chœurs doubles ;
* cithares, harpes, autres architectures de cordes pincées ;
* accordages non tempérés / micro-tonaux (table de notes au lieu de `+ fret`).

## 4. Règle de conduite

Ne pas complexifier le runtime actuel tant que ces instruments ne sont pas visés.
Quand la généralisation sera entreprise :

1. introduire `Voice`/`Course` **à côté** du modèle actuel (pas de big-bang) ;
2. réexprimer `frettedNote()` comme la table de positions par défaut d'une Voice
   *un chariot par corde* (rétro-compatible) ;
3. migrer `NoteAllocator` / `Capabilities` / le sélecteur corde-frette pour
   consommer les positions de la Voice ;
4. couvrir chaque étape par des tests de non-régression, profils v1/v2 inclus
   (voir la migration de profils, P1.12).
