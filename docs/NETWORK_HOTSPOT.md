# Hotspot & portail captif

> Comment atteindre la page de configuration quel que soit l'état du Wi-Fi, sans
> jamais rester « verrouillé dehors » : **portail captif** (la page s'ouvre toute
> seule quand on rejoint le Wi-Fi de l'ESP32) et **hotspot au bouton BOOT** (repli
> matériel). Inspiré de [Servo-Flute-GMB](https://github.com/glloq/Servo-Flute-GMB).
>
> Liés : [`WEB_INTERFACE.md`](WEB_INTERFACE.md) ·
> [`SAFETY.md`](SAFETY.md) · [`PIN_CONFIGURATION.md`](PIN_CONFIGURATION.md).

---

## 1. Deux façons d'atteindre la config

| Mode | Comment | Adresse |
|------|---------|---------|
| **Client (station)** | l'ESP32 rejoint votre box Wi-Fi | `http://<hostname>.local` (mDNS) ou l'IP attribuée |
| **Point d'accès (hotspot)** | l'ESP32 crée son propre Wi-Fi | rejoindre le réseau → **la page s'ouvre seule** (portail captif), sinon `http://192.168.4.1/` |

Le hotspot est le mode de **première configuration** et le **repli** si la station échoue (3 tentatives) — voir `Net.cpp`.

## 2. Portail captif (ouverture automatique de la page)

Quand le point d'accès est actif, rejoindre le Wi-Fi de l'instrument **ouvre
directement** la page de config, comme un portail Wi-Fi d'hôtel. Trois pièces
coopèrent :

1. **DNS wildcard** (`DNSServer` sur le port 53) : *toute* requête DNS résout vers
   l'IP de l'AP (`192.168.4.1`). Démarré à la montée de l'AP
   (`Net::startCaptivePortal`) et pompé à chaque tick (`Net::tick` →
   `dns_.processNextRequest()`), **uniquement en mode AP**.
2. **Redirections des sondes OS** : les URL que téléphones/PC testent pour détecter
   un accès Internet (`/generate_204`, `/gen_204`, `/hotspot-detect.html`,
   `/library/test/success.html`, `/connecttest.txt`, `/ncsi.txt`, `/redirect`,
   `/canonical.html`) renvoient une **redirection 302** vers `http://192.168.4.1/`
   au lieu du 204/succès attendu → l'OS affiche « se connecter au réseau » et ouvre
   la page (`WebApi::begin`).
3. **`onNotFound`** : en mode AP, toute autre URL est aussi redirigée vers le portail
   (attrape les sondes non listées) ; en mode station, un 404 normal.

En mode station, l'ESP32 n'est ni passerelle ni DNS : ces routes ne sont donc jamais
sollicitées et l'accès Internet du client reste normal.

## 3. Hotspot au bouton BOOT (repli matériel)

Si la config station est fausse (mauvais SSID / mot de passe), on pourrait se
retrouver sans aucun accès. Le **bouton BOOT** évite ça :

```
Maintenir BOOT (GPIO0) ~2 s  →  bascule immédiate en point d'accès + portail captif
```

- **Broche** : `GPIO0` (le bouton BOOT universel des cartes de dev ESP32),
  `INPUT_PULLUP`, actif à l'état bas.
- **Durée** : maintien ~2 s (`kBootHoldMs`) pour éviter les déclenchements
  accidentels.
- **Effet** : `Net::forceAccessPoint()` — coupe la station, monte l'AP et le portail
  **à chaud, sans redémarrer**. L'AP forcé **ne retente pas** la station (jusqu'au
  prochain reboot).
- Câblage : voir [`PIN_CONFIGURATION.md`](PIN_CONFIGURATION.md). `GPIO0` est une
  broche *strapping* — ne pas la maintenir **au démarrage** (cela entre en mode
  flash) ; l'appui se fait **après** le démarrage.

## 4. Depuis l'interface web

Le **modal de réglages** (bouton ⚙ de la barre du haut) → section *Hotspot* →
**« Start hotspot now »** appelle `POST /api/hotspot`, équivalent logiciel du bouton
BOOT (utile pour basculer en AP avant de changer les réglages réseau, sans risque de
se verrouiller). La station étant coupée, le client web est déconnecté : on rejoint
alors le réseau de l'instrument (la page se rouvre via le portail captif).

## 5. SSID / IP / sécurité

- **SSID de l'AP** : `network.apSsid` du profil (défaut `Stepper-Plucked-Strings-GMB`).
- **IP de l'AP** : `192.168.4.1` (adresse softAP par défaut de l'ESP32 ; aucune
  `softAPConfig` n'est posée). Si vous changez ce sous-réseau, mettez à jour les
  redirections du portail.
- **Mot de passe AP** : réglable dans le modal (write-only). Sans mot de passe (ou
  < 8 caractères), l'AP est **ouvert** — pratique pour la première config, mais
  pensez à définir un mot de passe WPA2 pour un usage durable (voir
  [`SAFETY.md`](SAFETY.md)).

## 6. Où c'est dans le code

| Élément | Fichier |
|---------|---------|
| DNS captif (start/stop/pompe) + `forceAccessPoint` | `firmware/src/platform/esp32/Net.{h,cpp}` |
| Redirections des sondes + `onNotFound` + `POST /api/hotspot` | `firmware/src/platform/esp32/WebApi.cpp` |
| Bouton BOOT (GPIO0) + service hotspot | `firmware/src/main.cpp` (`serviceHotspotRequests`) |
| Bouton « Start hotspot » + modal | `web-interface/js/settings.js`, `web-interface/js/api.js` |

## 7. Modes Setup et Performance (P1.11)

Deux **postures** cohérentes, obtenues avec les mécanismes déjà en place — pas un
protocole séparé (la mission recommande de ne pas complexifier si USB/DIN est le
transport principal).

### Setup (configuration / calibration)

* **AP + portail captif** disponible (§2) → la page s'ouvre seule.
* **Configuration web** et **calibration** (banc de test servo) accessibles.
* **Auth simplifiée** : tant qu'aucun token admin n'est défini, les écritures sont
  autorisées (bootstrap premier démarrage — `checkToken` renvoie vrai si le token
  stocké est vide). Le premier geste recommandé est de **définir un token admin**.

### Performance (jeu)

* **Administration protégée** : dès qu'un token admin est configuré, **toutes** les
  routes d'écriture (activation de profil, réglages, servo-test, reset…) exigent le
  token (`WebApi::authOk`). Les lectures (`/api/status`, `/api/diagnostics`) restent
  ouvertes.
* **USB / DIN prioritaire** quand disponible : l'abstraction transports (P1.7) permet
  d'alimenter le même `InstrumentController` par plusieurs entrées ; USB/DIN sont à
  privilégier en performance, le Wi-Fi UDP restant configurable.
* **`POST /api/panic` reste accessible sans authentification** — c'est **volontaire** :
  un arrêt d'urgence ne doit jamais dépendre d'un token (voir [`SAFETY.md`](SAFETY.md)).

### Durcissement MIDI UDP — kernel en place, activation à venir

Le MIDI UDP est non authentifié par nature. La **logique de filtrage** est désormais
implémentée et testée en hôte — `UdpSourceGate` (`core/net/UdpSourceGate.h`), tenu par
`MidiWifi` — avec trois postures :

* **`Open`** — accepte tout expéditeur (défaut ; comportement actuel inchangé) ;
* **`LockToFirst`** — **session/contrôleur reconnu** : verrouille sur le premier
  expéditeur, puis n'accepte plus que lui (un appareil pirate du réseau ne peut plus
  injecter de notes une fois qu'un contrôleur parle) ;
* **`Disabled`** — **désactivation du transport UDP** (jouer via USB/DIN en Performance).

Chaque datagramme refusé incrémente `MidiWifi::rejectedPackets()` (observable). Le gate
est **inerte** (`Open`) tant qu'un runtime/`DeviceConfig` (P1.13) n'appelle pas
`setSourcePolicy()` — comme pour les autres options réseau, l'activation doit arriver
**avec** le câblage qui l'honore, pas comme un simple champ (cf. P1.9/P1.10). Une
**whitelist d'IP** explicite (plusieurs expéditeurs autorisés) reste une extension
possible du gate. *Non validé sur socket réel* : seule la règle d'acceptation/refus est
testée en hôte ; le comportement réseau reste à valider au banc.

## 8. Dépannage — « le hotspot n'apparaît pas »

Le premier réflexe : **ouvrir le moniteur série à 115200 bauds** et redémarrer la
carte (bouton EN/RST). Le firmware trace tout le démarrage réseau :

```
[boot] Stepper-Plucked-Strings-GMB (reset: power-on)
[boot] profile: none valid -> CONFIG_SAFE (web UI only)
[net] hotspot "Stepper-Plucked-Strings-GMB" up (open) — http://192.168.4.1/
[boot] hold BOOT (GPIO0) ~2 s to force the Wi-Fi hotspot
[boot] setup complete — state configSafe
```

| Symptôme (série) | Cause / solution |
| ---------------- | ---------------- |
| Aucune sortie série | Mauvais port/baudrate, ou le firmware n'a pas été flashé (une compilation seule ne suffit pas — faire **Upload**). |
| La carte redémarre en boucle (`reset: brownout`) | Alimentation USB trop faible : le pic de courant Wi-Fi fait chuter le 3,3 V. Changer de câble/port USB ou alimenter en externe. |
| `[net] softAP(...) FAILED — retrying shortly` en boucle | La radio ne monte pas ; le firmware retente toutes les ~2 s. Vérifier l'alimentation ; si ça persiste, flash complet avec effacement (« Erase All Flash Before Sketch Upload »). |
| `[net] joining "<ssid>"...` au lieu du hotspot | Un profil **station** est stocké : l'AP de repli n'arrive qu'après 3 tentatives × 8 s (~24 s). Maintenir **BOOT ~2 s** pour forcer le hotspot tout de suite. |
| Rien ne se passe à l'appui sur BOOT | C'est un **maintien d'environ 2 s**, pas un appui bref. Et l'appui se fait **après** le démarrage : GPIO0 maintenu bas **à la mise sous tension** fait entrer la ROM en mode flash (broche de strapping) — la carte ne démarre alors pas le firmware. |
| « Too many redirects » à la connexion, ou page d'aide « upload the web UI » | Firmware compilé **avant** l'embarquement de l'UI (l'interface est désormais compilée dans le binaire — `WebAssets.cpp` — et servie même sans LittleFS). Recompiler/flasher depuis un arbre à jour ; la ligne série `[web] LittleFS /www absent — serving the embedded web UI` est alors normale. |

Rappels : le hotspot par défaut s'appelle **`Stepper-Plucked-Strings-GMB`** (ouvert,
sans mot de passe) ; la page de config est sur `http://192.168.4.1/`. Sur les cartes
ESP32-WROOM-32, le bouton marqué **BOOT** (parfois « IO0 ») est bien GPIO0 ; le bouton
**EN** est le reset.
