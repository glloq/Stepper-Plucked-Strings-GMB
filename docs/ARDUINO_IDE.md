# Ouvrir et compiler le projet dans l'IDE Arduino

Le firmware peut être construit **soit avec PlatformIO, soit avec l'IDE
Arduino** — c'est le même code source. Cette page décrit la voie Arduino IDE.

Le dossier `firmware/` est un *sketch* Arduino : il contient
[`firmware.ino`](../firmware/firmware.ino) (point d'entrée, du même nom que le
dossier) et un sous-dossier **`src/`** que la compilation Arduino traite
**récursivement**. Tout le firmware (cœur C++ pur + adaptateurs ESP32) est donc
compilé automatiquement. `setup()` et `loop()` se trouvent dans
`src/main.cpp` ; le fichier `.ino` reste volontairement vide. Le dossier
`test/` (tests natifs) est ignoré par l'IDE Arduino.

---

## 1. Pré-requis

* **Arduino IDE 2.x** (recommandé) — <https://www.arduino.cc/en/software>.
* La carte de référence **ESP32-S3-DevKitC-1** (ou une carte ESP32-S3
  équivalente).

## 2. Installer le support des cartes ESP32

1. `Fichier ▸ Préférences`.
2. Dans **« URL de gestionnaire de cartes supplémentaires »**, ajoutez :
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
3. `Outils ▸ Type de carte ▸ Gestionnaire de cartes…`, cherchez **esp32** et
   installez **« esp32 » par Espressif Systems** (version 3.x recommandée : le
   pilote LEDC des servos et l'API `ledcAttach` utilisés ici en dépendent).

## 3. Installer les bibliothèques

`Outils ▸ Gérer les bibliothèques…`, puis installez :

| Bibliothèque | Auteur / fork | Rôle |
| ------------ | ------------- | ---- |
| **ArduinoJson** (v7) | Benoît Blanchon | Profils JSON |
| **Adafruit PWM Servo Driver Library** | Adafruit | Servos via PCA9685 |
| **ESPAsyncWebServer** | ESP32Async (ou `mathieucarbou`) | Interface Web |
| **AsyncTCP** | ESP32Async | Dépendance de ESPAsyncWebServer |

> Les servos en **GPIO direct** n'utilisent que le cœur ESP32 (LEDC) ; Adafruit
> PCA9685 n'est nécessaire que si vous utilisez au moins un PCA9685. Les autres
> bibliothèques restent requises pour compiler.

## 4. Ouvrir le sketch

`Fichier ▸ Ouvrir…` puis sélectionnez **`firmware/firmware.ino`**.
L'IDE ouvre le sketch et affiche `firmware.ino` ainsi que l'arborescence
`src/`.

## 5. Choisir la carte et ses options

`Outils ▸ Type de carte ▸ esp32 ▸ **ESP32S3 Dev Module**`, puis réglez :

| Option | Valeur conseillée |
| ------ | ----------------- |
| USB CDC On Boot | **Enabled** (console série sur l'USB natif) |
| Flash Size | **8MB** (ou selon votre module) |
| Partition Scheme | un schéma **avec système de fichiers**, ex. *« 8M with spiffs (3MB APP/1.5MB SPIFFS) »* |
| PSRAM | selon la variante du module (OPI PSRAM si présente) |
| Upload Mode | UART0 / Hardware CDC |

> **Broches réservées** : GPIO19/20 (USB natif), 43/44 (UART0), 0/3/45/46
> (strapping), 48 (LED), 26–32 & 35–37 (Flash/PSRAM). Le firmware et l'interface
> Web les excluent automatiquement — voir [`PIN_CONFIGURATION.md`](PIN_CONFIGURATION.md).

## 6. Compiler et téléverser le firmware

Cliquez sur **Vérifier** (✓) pour compiler, puis **Téléverser** (→) carte
connectée en USB.

## 7. Téléverser l'interface Web (LittleFS)

L'interface est servie depuis LittleFS (`/www`). Elle se téléverse séparément :

1. Générez l'image du système de fichiers depuis `web-interface/` :
   ```bash
   cd firmware
   ./sync_web_data.sh        # copie web-interface/ -> firmware/data/www
   ```
   (Sous Windows sans Bash : copiez manuellement le contenu de `web-interface/`
   dans `firmware/data/www/`.)
2. Installez le plugin **arduino-littlefs-upload**
   (<https://github.com/earlephilhower/arduino-littlefs-upload>) : placez le
   `.vsix` dans `~/.arduinoIDE/plugins/` puis redémarrez l'IDE.
3. `Ctrl/Cmd + Shift + P ▸ **Upload LittleFS to Pico/ESP8266/ESP32**`.

## 8. Premier démarrage

À la mise sous tension, l'ESP32 crée le point d'accès Wi-Fi
**`Stepper-Plucked-Strings-GMB`**. Connectez-vous et ouvrez
`http://192.168.4.1` pour lancer l'assistant de configuration
(voir [`FIRST_CONFIGURATION.md`](FIRST_CONFIGURATION.md)).

---

## Dépannage

| Symptôme | Cause / solution |
| -------- | ---------------- |
| `fatal error: ArduinoJson.h: No such file or directory` | Bibliothèque non installée — voir §3. |
| `ledcAttach was not declared` | Cœur ESP32 en version 2.x — mettez à jour vers 3.x (§2). |
| Interface Web vide / 404 | Image LittleFS non téléversée — refaites §7 après `sync_web_data.sh`. |
| `Sketch too big` / pas de FS | Choisissez un *Partition Scheme* avec système de fichiers (§5). |
| Le sketch ne compile pas les fichiers de `src/` | Ouvrez bien `firmware/firmware.ino` (le `src/` doit être **à côté** du `.ino`). |
| Tests unitaires | Ils ne se compilent **pas** dans l'IDE Arduino ; utilisez `cd firmware/test && make` (voir [`ARCHITECTURE.md`](ARCHITECTURE.md)). |

## Équivalence PlatformIO

| Étape | Arduino IDE | PlatformIO |
| ----- | ----------- | ---------- |
| Compiler | Vérifier (✓) | `pio run` |
| Téléverser | Téléverser (→) | `pio run -t upload` |
| Système de fichiers | plugin LittleFS (§7) | `pio run -t uploadfs` |
| Moniteur série | Moniteur série | `pio device monitor` |
