# Cahier des charges — pointeur

Le **cahier des charges** faisant autorité pour Stepper-Plucked-Strings-GMB se
trouve à la racine du dépôt :

- 📄 [`../cahier des charges.md`](../cahier%20des%20charges.md) — spécification
  principale (architecture, GPIO, homing, notes, servos, machines d'état,
  allocation, MIDI, sécurité, phases, livrables).

Ce fichier est un simple renvoi : le contenu n'est **pas** dupliqué ici afin de
conserver une source de vérité unique.

## Spécifications complémentaires (racine du dépôt)

- 📄 [`../selection corde et frette.md`](../selection%20corde%20et%20frette.md) —
  sélection explicite de la corde et de la frette par MIDI CC (CC20/CC21).
- 📄 [`../Communication automatique des capacités par SysEx.md`](../Communication%20automatique%20des%20capacit%C3%A9s%20par%20SysEx.md) —
  protocole GMB d'annonce automatique des capacités par SysEx (blocs 1/5/6/7/8).

## Documentation dérivée (dossier `docs/`)

Les documents suivants résument et opérationnalisent le cahier des charges, en
lien avec le code implémenté dans `firmware/src/core/*` :

| Document | Objet | Références cahier des charges |
| -------- | ----- | ----------------------------- |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | modules, flux de données, snapshot de capacités, phases | §23, §24 |
| [`PIN_CONFIGURATION.md`](PIN_CONFIGURATION.md) | GPIO configurables, profils de cartes, conflits | §11 |
| [`WEB_INTERFACE.md`](WEB_INTERFACE.md) | niveaux d'interface, assistant, pages, API REST/WS | §9, §10, §18–20 |
| [`MIDI_PROTOCOL.md`](MIDI_PROTOCOL.md) | transport MIDI, sélection corde/frette, SysEx GMB | §8 + specs liées |
| [`CALIBRATION.md`](CALIBRATION.md) | pas/mm, homing, frettes, servos | §12–15 |
| [`SAFETY.md`](SAFETY.md) | états sûrs, panic, E-stop, alimentation | §21, §22 |
| [`FIRST_CONFIGURATION.md`](FIRST_CONFIGURATION.md) | guide débutant de l'assistant | §26 |
