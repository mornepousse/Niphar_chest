# Coffre Niphar — socle firmware et MSC brut

Design validé le 2026-08-06. Premier incrément du firmware du coffre.

## 1. Problème

Le matériel du coffre est conçu, revu et parti en fabrication ; le dépôt ne
contient que `README.md` et `docs/HARDWARE.md`. Il n'existe aucune ligne de
firmware, aucun build, aucun outillage.

Le coffre vise trois usages successifs — clé USB multi-ISO, token PGP/FIDO,
stockage amovible — mais aucun n'est atteignable tant que les deux chemins
matériels qui les portent tous ne sont pas prouvés : l'accès à la microSD, et
l'énumération en périphérique USB haute vitesse.

Le seul matériel disponible est le kit **JC-ESP32P4-M3-DEV**.

## 2. Ce que le kit prouve, et ce qu'il ne prouve pas

Le kit est un substitut valide pour cet incrément. La microSD y est câblée à
l'identique du coffre :

| Signal | Coffre (`docs/HARDWARE.md`) | Kit (BSP JCZN) |
|---|---|---|
| CLK | 43 | `BSP_SD_CLK` = 43 |
| CMD | 44 | `BSP_SD_CMD` = 44 |
| D0–D3 | 39, 40, 41, 42 | `BSP_SD_D0..D3` = 39, 40, 41, 42 |

Source : `1-Demo/IDF-DEMO/NoDisplay/common_components/espressif__esp32_p4_function_ev_board/include/bsp/esp32_p4_function_ev_board.h:71-76`,
et le `.c` associé qui monte en `SDMMC_HOST_SLOT_0`, `SDMMC_FREQ_HIGHSPEED`,
`SDMMC_SLOT_NO_CD` / `SDMMC_SLOT_NO_WP`. Conforme au silicium : « card one
(SDMMC_HOST_SLOT_0) signals are multiplexed with GPIO39–GPIO48 … via IO MUX »,
*ESP32-P4 Series Datasheet v0.7*, p. 81.

Le chemin USB device HS existe sur les deux : `SOC_USB_OTG_PERIPH_NUM 2`,
`SOC_USB_UTMI_PHY_NUM 1` (`components/soc/esp32p4/include/soc/soc_caps.h:485-489`,
ESP-IDF v5.5.2).

**Ce que le kit ne prouve pas.** Il a un CH340C et un bouton BOOTMODE ; le
coffre n'a ni bouton reset ni accès matériel au mode download. Sur le kit, un
firmware qui casse l'USB-Serial-JTAG se répare en trois secondes ; sur le
coffre, il se répare au fer à souder. Cette asymétrie ne se teste pas — elle se
tient par construction (§6).

## 3. Portée

Dans la portée : projet ESP-IDF, accès secteur à la microSD, énumération MSC
exposant la carte en brut, console de debug, outillage anti-régression.

Hors portée : multi-ISO, PGP/FIDO, liaison P4↔C6, OTA. Chacun aura sa propre
spec.

## 4. Architecture

Principe structurant : **le MSC possède la carte SD, exclusivement.** Tant que
le firmware expose des blocs bruts à l'hôte, il ne monte pas FATFS de son côté.
Deux systèmes de fichiers qui écrivent le même média sans se concerter
corrompent le média — ce n'est pas un risque, c'est une certitude. `sd_card`
sert des secteurs, rien de plus.

```
main/
├── board.h              pinout + garde-fous compile-time
├── main.c               app_main : sd → usb → console
├── storage/sd_card.*    SDMMC slot 0, 4-bit, accès secteur
├── usb/usb_device.*     esp_tinyusb, port HS, descripteurs
├── usb/msc_disk.*       callbacks tud_msc_* → secteurs SD
└── console/console.*    esp_console sur USB-Serial-JTAG
```

### Frontières

**`sd_card`** — détient le `sdmmc_card_t*`. Ne connaît ni l'USB ni FATFS.

```c
esp_err_t sd_probe(void);                 /* (re)détecte la carte */
bool      sd_present(void);
uint32_t  sd_sector_count(void);
uint32_t  sd_sector_size(void);
esp_err_t sd_read_sectors (void *dst, uint32_t start, uint32_t count);
esp_err_t sd_write_sectors(const void *src, uint32_t start, uint32_t count);
```

Pas de card-detect matériel sur le coffre : `sd_probe()` est appelé au boot et
re-déclenchable depuis la console. La règle d'usage actée — carte insérée et
retirée hors tension — rend inutile toute tâche de polling à ce stade.

**`msc_disk`** — implémente les callbacks TinyUSB (`inquiry`,
`test_unit_ready`, `capacity`, `read10`, `write10`, `start_stop`). Ne connaît
que l'interface ci-dessus. Signale *no medium* quand `sd_present()` est faux,
plutôt que d'échouer bruyamment.

**`usb_device`** — init esp_tinyusb sur le port HS, descripteurs, VID/PID.
Ignore tout de la carte SD.

**`console`** — REPL `esp_console` sur l'USB-Serial-JTAG uniquement. Commandes
`sd info`, `sd probe`, `usb status`.

### Flux de données

```
hôte USB ──HS──> TinyUSB ──> msc_disk ──> sd_card ──SDMMC 4-bit──> microSD
hôte USB ──FS──> USB-Serial-JTAG ──> console ──> sd_card (lecture seule)
```

Les deux chemins USB sont physiquement distincts (contrôleurs séparés, ports de
hub séparés sur le coffre). Leur indépendance est une propriété à vérifier, pas
à supposer : voir §7.

### Deux pièges connus

1. **Buffers DMA.** `sdmmc_read_sectors()` exige de la mémoire DMA-capable ;
   les buffers que TinyUSB présente aux callbacks ne le sont pas
   nécessairement, et `read10_cb` peut arriver à un offset non aligné sur un
   secteur. `msc_disk` passe par un bounce buffer `MALLOC_CAP_DMA` et gère les
   offsets et longueurs partiels.
2. **VID/PID.** Le PID doit être distinct de celui de `KeSp_firmware` pour ne
   pas troubler les règles udev et les clients côté hôte.

## 5. Gestion des erreurs

- Absence de carte au boot : log en warning, `sd_present()` faux, le device USB
  énumère quand même et répond *medium not present*. Le coffre reste flashable
  et interrogeable — jamais de panique au démarrage pour une carte absente.
- Erreur de lecture ou d'écriture secteur : remontée en erreur SCSI à l'hôte,
  log au niveau erreur. Pas de retry silencieux qui masquerait une carte
  mourante.
- Échec d'init USB : log en erreur, la console reste vivante. La console est le
  dernier recours de diagnostic ; rien ne doit pouvoir l'emporter.

## 6. Garde-fous du coffre

Le coffre n'a pas de bouton reset ni de mode download matériel. Deux règles en
découlent, et aucune ne se vérifie à l'exécution sur le kit :

- **Ne jamais réaffecter GPIO24/25** (USB-Serial-JTAG). `board.h` porte un
  `_Static_assert` par pin déclaré ; `scripts/check.sh` échoue si le code
  réaffecte ces GPIO.
- **Ne jamais entrer en deep-sleep permanent.** `scripts/check.sh` échoue sur
  l'apparition de `esp_deep_sleep_start`.

La console est figée sur l'USB-Serial-JTAG dans `sdkconfig.defaults` : le
coffre n'a pas d'UART accessible.

## 7. Vérification

Sur le kit, console via USB-Serial-JTAG :

- `sd info` → capacité cohérente avec la carte, bus 4-bit, fréquence négociée.
- `sd probe` sans carte → absence signalée proprement, pas de panique.

Sur l'hôte, câble branché sur le port HS :

- `lsblk` montre un disque de la bonne taille.
- Secteur 0 lisible (`dd` + `xxd`), signature MBR si la carte est partitionnée.
- Montage lecture seule, lecture d'un fichier connu, démontage.
- Écriture : copie d'un fichier, `sync`, démontage, remontage, relecture —
  contenu identique.
- **Coexistence** : pendant que le disque est monté sur l'hôte, la console
  répond toujours. C'est ce test, et lui seul, qui prouve que les deux chemins
  USB sont indépendants.

Dette de vérification assumée, à lever quand le coffre arrivera : comportement
de récupération sans bouton, et liaison P4↔C6.

### Résultats du bring-up (2026-08-06, kit JC-ESP32P4-M3-DEV, carte SE04G 4 Go)

Tout est vert côté fonctionnel : `INQUIRY` et `READ CAPACITY` corrects vus du
noyau (`Direct-Access Niphar Coffre microSD`, 3,64 Gio), partitionnement et
formatage depuis l'hôte, 64 Mio écrits puis relus **après démontage** avec des
empreintes SHA-256 identiques. Le chemin USB → MSC → SDMMC est prouvé dans les
deux sens.

La question **VDDPST_5** est tranchée sur le kit : la carte est détectée par le
chemin *alimentation externe*, sans le LDO interne — conforme à l'hypothèse du
coffre. Reste à confirmer sur la netlist que le coffre relie bien ce pin au
3,3 V ; le kit ne prouve que le kit.

Débits :

| | avant | après | SDMMC brut |
|---|---|---|---|
| lecture | 5,8 Mio/s | **9,4 Mio/s** | 18,3 Mio/s |
| écriture | 2,4 Mio/s | **5,3 Mio/s** | non mesuré |

Le facteur était `CFG_TUD_MSC_EP_BUFSIZE` : TinyUSB réclamait la carte par blocs
de 4 Kio, porté à 32 Kio. L'hypothèse initiale — un tampon de rebond qui
dominerait — était fausse, et les compteurs de `msc_disk` l'ont montré :
0 secteur rebondi sur 4650, chemin DMA direct à 100 %.

Il reste un facteur deux en lecture (9,4 contre 18,3 Mio/s). La cause est
structurelle : chaque bloc est traité **synchroniquement**, l'USB attend le
SDMMC sans recouvrement. Le combler demande un double tampon — une vraie
refonte de `msc_disk`, à décider comme un incrément à part.

Pour l'écriture, 5,3 Mio/s est plausible pour une SE04G d'entrée de gamme ; ce
n'est pas démontré, faute d'une mesure d'écriture SDMMC brute (elle détruirait
le contenu de la carte). À reprendre sur une carte sacrifiable.

## 8. Décisions et alternatives écartées

| Décision | Alternative écartée | Raison |
|---|---|---|
| Cible de build unique | deux boards `boards/<name>/` | zéro delta de pinout entre kit et coffre ; deux `board.h` jumeaux à garder synchro ne protègent de rien |
| Callbacks `tud_msc_*` maison | composant `tinyusb_msc_storage` | le composant est pensé « une SD = un volume » ; le multi-ISO à média commutable et le composite CCID+HID exigent le contrôle des callbacks. Écrire la bonne couche tout de suite évite de la jeter |
| Dépendance sur `espressif/tinyusb` brut | wrapper `espressif/esp_tinyusb` | découvert à l'implémentation, voir §9 |

## 9. Correction — pourquoi TinyUSB brut

La décision « callbacks maison » avait été chiffrée en supposant qu'on garderait
`espressif/esp_tinyusb` pour le PHY, la tâche et les descripteurs. C'est faux :
le wrapper ne sépare pas la classe MSC de sa propre couche de stockage.

- `CFG_TUD_MSC` dérive de `CONFIG_TINYUSB_MSC_ENABLED`, et ce même Kconfig
  décide de compiler `tinyusb_msc.c`, qui définit `tud_msc_read10_cb` et
  consorts en **symboles forts** — collision directe avec les nôtres.
- Contourner par injection de macros à la compilation fonctionne jusqu'à
  l'édition de liens : `tinyusb.c` appelle `msc_storage_mount_to_usb()` en dur
  depuis `tud_mount_cb()`. Aller plus loin voudrait dire forger les symboles
  internes du composant — un prix que le projet ne doit pas payer.

Le projet dépend donc directement de `espressif/tinyusb`, avec son propre
`main/tusb_config.h` et, dans `main/usb/usb_device.c`, l'initialisation du PHY
UTMI (`usb_new_phy`), une tâche `tud_task()` et les callbacks de descripteurs.
Le coffre étant alimenté par le bus, tout le monitoring VBUS du wrapper est sans
objet — ce qui réduit nettement ce qu'il fallait reprendre.

Coût réel : environ 120 lignes de plomberie, en échange du contrôle total des
callbacks et d'une dépendance de moins.
| MSC propriétaire exclusif de la SD | montage FATFS simultané côté firmware | double accès concurrent au même média = corruption |
