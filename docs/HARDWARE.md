# Contrat matériel — coffre ESP32-P4 (Niphargus v2)

Source : feuille `p4.kicad_sch` du dépôt Niphargus, vérifiée pin par pin à la netlist
(revue 2026-08-06). Le module est un **JC-ESP32P4-M3 V0.2** (U16).

## Alimentation — filaire only

- AMS1117-3.3 (U17) : +5V USB → rail `P4_3V3`. **Le coffre n'existe pas sur batterie**
  (décision design : régulateur 5 mA de repos assumé car alimenté seulement par USB).
- CHIP_PU : RC 10k → P4_3V3 + 1 µF → GND : boote automatiquement quand l'USB arrive.
  **Pas de bouton reset ni d'accès au mode download matériel** — la récupération d'un
  firmware qui casse l'USB-Serial-JTAG impose de souder sur le module (ne JAMAIS
  réaffecter GPIO24/25, ne jamais bloquer le deep-sleep permanent).

## USB — deux chemins distincts

| Chemin | Pins module | Rôle | Hub CH334R |
|---|---|---|---|
| PHY USB 2.0 OTG **HS** | 39/40 (`ESP_USB_N/P`, chip USB_DM/DP 49/50) | Data/MSC/HID — le produit | port 2 (permanent) |
| **USB-Serial-JTAG** | 42/43 (`USB1_P1_N/P` = GPIO24/25) | Flash/debug (esptool sans strap) | port 3 via jumpers **JP1/JP2** (pontés par défaut, ouvrables pour isoler) |

Le hub (CH334R, mode sans quartz) est alimenté par le 5 V USB : hors filaire, tout ce
sous-système est mort — c'est voulu.

## microSD — SDMMC IOMUX fixe

| Signal | GPIO | Pull-up |
|---|---|---|
| CLK | 43 | — (jamais de pull-up sur CLK) |
| CMD | 44 | 10k |
| D0–D3 | 39, 40, 41, 42 | 10k chacun |

- Connecteur Würth 693072010801 (charnière) — **pas de card-detect** : détection
  logicielle par polling CMD.
- **Règle d'usage actée : cartes insérées/retirées HORS TENSION uniquement**
  (pas de TVS sur les lignes SD — décision Mae 2026-08-06).
- Vitesse : IO fixe 3,3 V (pas de commutation 1,8 V) → High-Speed 50 MHz max
  (~20-25 Mo/s réels), pas de SDR104. Pas de résistances série (à garder en tête
  si problèmes d'intégrité au bring-up).

### Alimentation des IO SD — réglé, rien à câbler

Sur ESP32-P4, les IO de la carte ne sont pas alimentées par le rail de la carte
SD mais par un rail dédié du SoC :

> « ESP32-P4 SDMMC Host requires the IO voltage to be supplied externally via
> the **VDDPST_5 (SD_VREF)** pin. If the design doesn't require the higher speed
> SD modes, this pin can be simply connected to the 3.3V supply. »
> — ESP-IDF Programming Guide, *SDMMC Host Driver* (ESP32-P4),
> § Configuring Voltage Level.

Attention au nom : le datasheet appelle ce rail **`VDD_IO_5`** (pin 85 du
boîtier), et c'est bien lui qui alimente GPIO39-48 — table 2-1 « Pin Overview »,
*ESP32-P4 Series Datasheet v0.7* p. 16. Chercher « VDDPST » dans un symbole ne
donne rien.

**Ce rail est interne au module JC-ESP32P4-M3** : il n'apparaît pas sur le
schéma du coffre et il n'y a rien à y relier. Le VDD du connecteur microSD, lui,
vient du régulateur de la carte — deux choses distinctes.

Vérifié empiriquement le 2026-08-06 sur le kit de dev, qui embarque le même
module : la carte est détectée par le chemin « alimentation externe », sans le
LDO interne canal 4. C'est cohérent avec `LDO_VO4 non câblé` et avec des IO
fixes 3,3 V sans SDR104. Le firmware sonde quand même les deux chemins au
démarrage et journalise celui qui a servi (`sd info`) — si un jour un module
différent était monté, l'écart se verrait au premier boot.

## Lien S3↔coffre — SPI, le coffre en esclave

Décidé le 2026-08-06. Le PCB était encore éditable ; le brochage ci-dessous est
celui retenu côté coffre.

| signal | coffre (P4) | S3 (clavier) |
|---|---|---|
| CS | GPIO7 | GPIO3 |
| MOSI | GPIO8 | MOSI de SPI2, déjà routé |
| SCK | GPIO9 | SCK de SPI2, déjà routé |
| MISO | GPIO10 | MISO de SPI2, déjà routé |
| IRQ (coffre→S3) | GPIO11 | GPIO46 |

C'est le **quatuor IOMUX natif de SPI2** sur P4 (`spi_slave.rst:157-162`, valeurs
`esp32p4`), donc chemin direct sans matrice GPIO. Ça compte : le driver bascule
tout le bus sur la matrice dès qu'un seul signal est hors IOMUX, et la matrice
allonge le retard d'entrée de MISO — sur un faisceau entre deux cartes, cette
marge n'est pas de trop.

Côté S3, le coffre est un troisième client du bus SPI2 déjà partagé entre le
NRF24 et l'e-ink (`KeSp_firmware`, `boards/kase_half_left/board.h:52-58,78-79`),
d'où un coût d'un seul pin : le CS.

- **Mode 0 obligatoire** (CPOL=0, CPHA=0), pas par préférence : c'est ce qui
  garde SCK au repos à l'état bas, condition qui rend déjà sûr le `GPIO_NUM_45`
  de strapping côté S3 (`VDD_SPI` : haut au reset ferait passer le rail flash du
  clavier en 1,8 V).
- **IRQ active à l'état haut, pull-down côté S3.** Le coffre est non alimenté la
  plupart du temps ; un pull-up injecterait du courant dans un rail éteint par
  les diodes de protection du P4. GPIO46 est bien un strap du S3, mais il ne
  contrôle que l'impression des messages ROM et son niveau est « Ignored » avec
  l'eFuse par défaut (ESP32-S3 TRM v1.8, table 8.3-1, p. 536).
- **GPIO35 interdit** — c'est le seul strap qui décide entre boot applicatif et
  mode download (TRM table 11.2-2). GPIO34/36/37/38 restent libres et sans effet
  sur le boot tant que GPIO35 est haut.
- **Ligne d'interruption présente** (GPIO11 → GPIO46), ajoutée après coup. Le
  coffre peut donc réclamer l'attention du clavier, ce qui rendra possible une
  invite à l'écran plutôt qu'un simple relais de confirmation.

Sur le kit de dev, GPIO7-11 sont pris (I2C du codec sur 7/8, I2S sur 9/10,
ampli sur 11) : le lien n'y est pas câblable. C'est la première divergence
réelle de brochage entre les deux cartes, d'où le découpage `boards/`.

## L’hôte peut forcer le mode download — tranché

Le contrat dit « pas d'accès au mode download **matériel** », ce qui est exact —
et masque une voie logicielle qui, elle, est ouverte à l'hôte :

> « if the download mode flag is set when ESP32-P4 is reset, ESP32-P4 will
> reboot into download mode »
> — ESP32-P4 TRM, chap. 53, table 53.3-2, p. 2715 : sur la CDC-ACM de
> l'USB-Serial-JTAG, `RTS=0 / DTR=1` pose le drapeau, `RTS=1 / DTR=0` reset.

Confirmé au chap. 11 (p. 798) : « USB Serial/JTAG Controller can also force
switch the chip to Joint Download Boot mode from SPI Boot mode », désactivable
seulement par `EFUSE_DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE`. Vérifié aussi
empiriquement le 2026-08-06 : une séquence de reset qui tirait DTR bas a fait
démarrer le kit en `boot:0x16 DOWNLOAD` au lieu de lancer l'application.

**Aujourd'hui, sans conséquence** : l'hôte peut déjà tout lire et tout écrire par
le MSC, et n'importe quel logiciel de l'hôte peut de toute façon reflasher.

**Demain, ça invalide une hypothèse.** Le modèle hérité de KeSp accepte des clés
en clair en NVS *parce que* leur extraction demanderait un accès physique. Ici,
du logiciel hôte entre en download boot et dump la flash entière, NVS comprise.

### Tranché le 2026-08-07 : les jumpers JP1/JP2, retirés en production

**Décision de Mae.** Le coffre part sans ses jumpers JP1/JP2. L'USB-Serial-JTAG
est alors physiquement hors de portée de l'hôte, donc le déclencheur du mode
download aussi.

Ce qui rend cette solution meilleure que les deux autres envisagées (brûler
`EFUSE_DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE` + Secure Boot, ou accepter le risque) :
**le jumper est à la fois la frontière de sécurité et la voie de récupération.**
Un attaquant logiciel n'a aucun chemin ; toi, avec la carte en main, tu
repositionnes le jumper et tu reflashes. La récupération redevient possible,
simplement délibérée — là où les eFuses l'auraient supprimée pour toujours.

Deux précisions utiles.

**Le dump, lui, passe par les deux ports.** Une fois la puce en mode download,
la ROM sert le téléchargement aussi bien par l'USB-Serial-JTAG que par l'OTG
haute vitesse — *ESP32-P4 Series Datasheet v0.7*, p. 37 : « USB Download Boot:
USB-Serial-JTAG Download Boot, USB 2.0 OTG Download Boot ». Ce qui est fermé,
c'est le **déclenchement** : les lignes RTS/DTR de la CDC-ACM de
l'USB-Serial-JTAG (TRM chap. 53, table 53.3-2, p. 2715). Le firmware n'expose
aucune CDC sur le port haute vitesse — que du MSC, du CCID ou du HID — donc rien
à y manipuler.

**La console disparaît avec les jumpers.** `usb mode` et `sec confirm` ne seront
plus atteignables depuis l'hôte sur un coffre de production. C'est cohérent :
sur le coffre ces deux commandes viennent du S3 par le lien SPI, pas de la
console. Le kit de dev, lui, garde ses jumpers et sa console.

## Ce qui ne se teste pas sur le kit de dev

Le JC-ESP32P4-M3-DEV câble la microSD à l'identique (vérifié pin par pin sur le
BSP du fabricant), donc tout le code de stockage se valide dessus. Trois choses
n'y seront jamais éprouvées, parce que **le kit pardonne et le coffre non** :

| | Kit de dev | Coffre |
|---|---|---|
| Récupération | CH340C + bouton BOOTMODE | rien — fer à souder |
| C6 | `U0RXD`/`U0TXD`/`IO9` câblés | NC |
| Alimentation | 5 V USB **ou** Li-ion (TLV62560) | USB seul |

Conséquence directe : les règles « jamais de GPIO24/25 réaffectés » et « jamais
de deep-sleep permanent » ne peuvent pas être vérifiées à l'exécution. Elles
tiennent par construction — `_Static_assert` dans `main/board_common.h` et greps dans
`scripts/fast.sh` — et ces garde-fous ne doivent pas être contournés « juste
pour un test ».

## Validation OpenPGP CCID — 2026-08-07

Tâche 12 du portage sécurité (`.superpowers/sdd/2026-08-07-portage-ccid-otp/`),
sur le kit JC-ESP32P4-M3-DEV, hôte gpg 2.4.9 / scdaemon (pilote CCID interne,
`pcscd` **inactif**, volontairement).

**Prouvé** :
- `usb mode pgp` puis `gpg --card-status` voit la carte : Application ID, série
  dérivé de la MAC, attributs de clé (`nistp256 cv25519 nistp256`). Tout le
  chemin CCID → APDU → objets de données OpenPGP fonctionne.
- **Exclusivité** : `lsblk` ne montre aucun disque pendant que gpg dialogue
  avec la carte — un seul jeu de descripteurs USB à la fois, comme conçu
  (`usb/usb_mode.c`).
- **Confirmation physique exigée**, éprouvée dans les deux sens sur une clé de
  signature générée sur la carte (`GENERATE 0x47/80`, `PSO:CDS`
  `00 2A 9E 9A`, testé via `SCD PKSIGN` brut plutôt que le flux `--card-edit`
  complet — voir « écarts » ci-dessous) :
  - **sans** `sec confirm` : le worker CCID envoie une trame CCID
    d'extension de temps (WTX) toutes les ~1,5 s pendant que le firmware
    attend l'appui ; après exactement 15 s (`SEC_CONFIRM_TIMEOUT_MS`), la
    carte répond `SW=6985` (Conditions of use not satisfied) et scdaemon
    rapporte l'échec.
  - **avec** `sec confirm` tapé pendant la fenêtre d'attente : la carte
    répond `SW=9000` avec une signature ECDSA P-256 de 64 octets, quelques
    centaines de ms après l'appui simulé. `Signature counter` passe à 1.
  - Les deux moitiés comptent : sans la première, rien ne prouve que la
    confirmation soit réellement requise ; sans la seconde, rien ne prouve
    qu'elle suffise.

**Pas prouvé** : l'échange CR-HMAC réel du mode OTP-HID (voir tâche 11 —
vérifié à l'énumération USB seulement, faute d'outillage HID sur ce poste).
Ne pas documenter l'OTP comme validé tant que ce test manque.

### Trois bugs trouvés en validant, absents des tâches précédentes

Cette tâche n'était censée toucher aucun code (voir son brief) ; les trois
correctifs suivants sont apparus en essayant de faire fonctionner
`gpg --card-status`, pas en cherchant des bugs :

1. **`openpgp_do_init()`/`openpgp_card_load()` jamais appelés** (signalé par
   la tâche 10) — le magasin de DO démarrait vide. Câblé à l'entrée du mode
   PGP (`usb/mode_pgp.c:mode_pgp_data_load()`, appelée par `usb_mode.c` après
   `usb_device_install()`), pas au démarrage : voir `CLAUDE.md` pour le choix.
2. **`nvs_flash_init()` jamais appelé nulle part** dans le firmware — toute
   écriture NVS (DO, PIN, clés) échouait `ESP_ERR_NVS_NOT_INITIALIZED`.
   Silencieux tant que rien ne lisait/écrivait vraiment la NVS (avant que
   ccid.c ne rende `openpgp_card_apdu` atteignable, cf. tâche 10). Ajouté dans
   `main.c`, avant l'USB.
3. **Descripteur CCID non conforme USB 2.0 en haute vitesse** :
   `KASE_CCID_ITF_DESC` figeait `wMaxPacketSize` à 64 octets pour les deux
   points bulk, aux deux vitesses — copié tel quel depuis KeSp (un dongle
   ESP32-S3, jamais éprouvé à cette vitesse pour CCID). Le coffre négocie la
   haute vitesse (480 Mbps), où l'USB 2.0 impose 512 comme SEULE valeur
   légale pour un point bulk (tableau 5-5 de la spec). Le descripteur non
   conforme laissait l'énumération passer mais corrompait les échanges bulk
   côté hôte (`scdaemon --debug-ccid-driver` : « unexpected bulk-in msg type
   (00) », puis timeout). Corrigé en paramétrant la taille (64 en FS, 512 en
   HS), sur le modèle déjà en place pour `TUD_MSC_DESCRIPTOR` dans
   `mode_storage.c`.

   Un quatrième symptôme lié au même défaut de conformité DMA/cache : les
   tampons statiques de `ccid.c` (`s_out_buf`/`s_in_buf`/`s_wtx_buf`)
   n'étaient pas alignés sur la ligne de cache (64 o), requis sur ESP32-P4
   pour que `esp_cache_msync()` garde le CPU et le DMA USB cohérents. Log
   observé : `cache: esp_cache_msync(112): start address ... not aligned`,
   aux adresses exactes de ces tampons (vérifié via `nm` sur l'ELF). Corrigé
   avec `CFG_TUSB_MEM_ALIGN` (déjà défini dans `tusb_config.h`, utilisé
   nulle part ailleurs dans le code porté).

   Les deux — endpoint 64 o en HS et tampons non alignés — se manifestaient
   par le même symptôme observable côté hôte (réponses CCID corrompues) ;
   corriger l'un sans l'autre n'aurait pas suffi. **KeSp (ESP32-S3) n'a jamais
   ce problème** : le S3 n'a pas la même architecture de cache-cohérence DMA
   que le P4, d'où l'absence de tout précédent amont sur ces deux points.

## Divers

- **C6 embarqué du module : vierge et non câblé** (U0RXD/U0TXD/IO9 NC) — flashable
  uniquement via le P4 (esp-hosted / OTA). IO9 flottant = boot normal, sûr.
- Straps P4 (GPIO34-38) : tous NC ; GPIO35 a un pull-up interne → SPI boot par défaut.
- 9 pins GND du module câblés ; DSI/CSI/LDO_VO4 non câblés (assumé).
- Lien direct S3↔P4 : AUCUN (abandonné) — le P4 parle à l'hôte par l'USB, point.

## Carte-clé WT9932P4-TINY

Troisième carte du projet — une clé de sécurité autonome, sans microSD, sans
lien vers un clavier : deux boutons et une LED adressable en façade en tiennent
lieu. Brochage lu sur le schéma constructeur (`WT9932P4-TINY_1v2`, JLCEDA,
révisé 2025-08-07).

**⚠️ Brochage non vérifié au matériel.** Le schéma a été lu sur un rendu
d'image du fabricant (1920 px), pas sur le PCB ni sur une netlist exportée —
contrairement au tableau du coffre en tête de ce document, qui lui est vérifié
pin par pin. Les boutons IO32/IO33 ne sont pas encore soudés sur l'exemplaire
de Mae. **À recouper avec la sérigraphie du module avant tout câblage** —
seul le port USB-Serial-JTAG a été réellement exercé (tâche 6, voir plus bas) :
`chip_id` puis un flash complet y sont passés avec succès. L'OTG HS (J4) n'a
transporté aucun trafic — le firmware est resté en `USB_MODE_NONE` tout du
long — et le strap de boot n'a pas été testé en pratique (aucun appui sur le
bouton BOOT, la mise en mode download utilisée pour flasher passe par
RTS/DTR logiciel sur l'USB-Serial-JTAG, pas par IO35).

| Élément | Pin(s) | Détail |
|---|---|---|
| LED adressable WS2812 | IO51 (DIN) | Alimentée en 5 V, données en 3,3 V — sous le seuil VIH strict (0,7×VDD = 3,5 V). Voir avertissement matériel ci-dessous. |
| LED témoin d'alimentation | R13 (1 kΩ) | Non pilotable, câblée en direct. |
| Boutons utilisateur | IO32 (MODE), IO33 (CONFIRM) | Vers la masse, pull-up interne, aucun composant externe. Pas encore soudés — à câbler par Mae. |
| Bouton BOOT | IO35 | Strap de boot du P4 — **non exposé au firmware**, voir ci-dessous. |
| RESET | CHIP_PU | RC de mise sous tension, pas de bouton reset dédié (comme le coffre). |
| J4 | OTG HS | PHY USB 2.0 haute vitesse — le port produit (MSC/CCID/HID). |
| J3 | USB-Serial-JTAG | Seul chemin de flash/debug (GPIO24/25) — mêmes règles que le coffre. |
| microSD | — | Aucun connecteur au schéma : `BOARD_HAS_SD 0`. |

### Pourquoi GPIO35 (bouton BOOT) reste hors du firmware

GPIO35 est l'un des cinq straps de boot de l'ESP32-P4 : « ESP32-P4 has five
strapping pins: GPIO34, GPIO35, GPIO36, GPIO37, GPIO38 » — *ESP32-P4 TRM*,
chap. 11.2, p. 795. Son niveau au reset décide seul entre boot applicatif et
mode download : table 11.2-2, p. 796 — `GPIO35 = 1` (défaut, pull-up interne)
sélectionne le SPI Boot mode, `GPIO35 = 0` sélectionne le Joint Download Boot,
indépendamment de GPIO36/37/38.

Le silicium autoriserait pourtant sa réutilisation après coup : « After the
reset is released, the strapping pins work as normal-function pins » (même
page, §11.2.1 / 11.2.2). La carte-clé s'en prive quand même — trois raisons :

1. Un appui pendant la mise sous tension forcerait le mode download au lieu de
   démarrer l'application : la clé disparaîtrait du bus au lieu de s'annoncer.
2. Un reset accidentel bouton enfoncé (CHIP_PU repasse bas puis haut) produit
   le même effet en cours d'usage.
3. `scripts/fast.sh` (garde-fou n°1) exclut déjà les en-têtes de carte de son
   grep sur `GPIO_NUM_(24|25|35)` — un usage de GPIO35 dans `boards/wt9932_key/
   board.h` resterait donc vert pendant que le sens de la broche s'y
   inverserait, de « réservée » à « bouton utilisateur ». Le firmware ne doit
   pas dépendre d'un garde qui ne le verrait pas.

IO32 et IO33 ont été choisies à la place parce que ce sont, avec IO26 à IO31,
les seules broches du P4 dont les trois colonnes de la table GPIO sont vides —
ni fonction analogique, ni LP GPIO, ni restriction de commentaire (*ESP-IDF
Programming Guide*, « GPIO & RTC GPIO — ESP32-P4 », § GPIO Summary). Elles
sortent sur J7 et n'empiètent sur aucun bloc occupé par les cartes sœurs
(microSD sur GPIO39-48, lien S3 sur GPIO7-11).

### Cette carte ne résiste pas au dump de flash

Contrairement au coffre de production (jumpers JP1/JP2 retirés, voir plus
haut), **rien n'isole physiquement l'USB-Serial-JTAG de l'hôte sur la
carte-clé** : le bouton BOOT est en façade, et déclencher le mode download
logiciel par RTS/DTR reste également ouvert (TRM chap. 53, table 53.3-2, p.
2715, cf. « L'hôte peut forcer le mode download » ci-dessus). Un hôte — ou
quiconque manipule physiquement la clé — peut dumper la flash entière.

C'est significatif ici parce que la flash contient les clés privées OpenPGP en
clair (mode PGP, `usb/mode_pgp.c`) : un dump complet les expose. La parade
retenue pour le coffre (retirer les jumpers en production) ne s'applique pas à
cette carte — elle n'a pas de jumpers équivalents à retirer, et son propos
(clé transportable, boutons + LED en façade) suppose justement un boîtier
accessible. Aucune mitigation n'est proposée ici ; c'est un écart de sécurité
connu, à traiter séparément (Secure Boot / Flash Encryption, hors-scope des
`sdkconfig.defaults*` partagés par garde-fou n°3 de `fast.sh`).

### Validé sur matériel — 2026-08-16

Tâche 6 de `.superpowers/sdd/2026-08-16-carte-cle-wt9932/`, module WT9932P4-TINY
réel (MAC `30:ed:a0:e0:bc:5f`), en écrasant le firmware `jc_devkit` qui y était
précédemment flashé.

- `chip_id` confirme la puce avant tout flash : ESP32-P4 rev v1.0, MAC
  `30:ed:a0:e0:bc:5f` — donc le port `by-id …-30:ED:A0:E0:BC:5F-if00` désigne
  bien ce module, pas une autre carte de la machine de dev.
- Boot mesuré par reset RTS seul (DTR haut, jamais de strap en mode download)
  et lecture brute du port, sans TTY : `main_task: Calling app_main()` à
  t=832 ms, tous les logs de `app_main()` (carte, microSD sautée, `sec_gate`)
  au même tick, prompt `niphar>` atteint. Avec un terminal qui répond à la
  sonde ANSI de linenoise (`ESC[5n`, comme le ferait tout terminal
  interactif), `main_task: Returned from app_main()` tombe à t=919 ms — un
  gain net face aux ~11 s d'avant (sondage SD absent qui timeoutait). Voir
  « écart de mesure » ci-dessous.
- `lsusb` : aucun `303a:4021` (le VID:PID composite MSC/CCID/HID) — seul
  `303a:1001` (USB-Serial-JTAG) apparaît, confirmant `USB_MODE_NONE` au
  démarrage sur cette carte aussi.

**Écart de mesure, pour qui reproduit avec `boot_capture.py` tel quel** : ce
script ne répond jamais à la requête de statut ANSI (`ESC[5n`) que
`esp_console_setup_prompt()` envoie pour détecter un terminal capable
(`linenoiseProbe()`, `components/console/linenoise/linenoise.c`, budget
500 ms). Sans réponse, la sonde épuise son budget et le prompt bascule en
mode « dumb » — mesuré à un delta constant et reproductible de 1000 ms entre
`Calling app_main()` et `Returned from app_main()` sur trois essais (832→1832,
819→1819, 832→1832). C'est un artefact du script de capture non interactif,
pas une régression du firmware : dès qu'un répondant existe côté hôte (testé
en renvoyant `ESC[0n` sur réception de la sonde), le même boot tombe à 100 ms
(819→919). Les deux nombres sont sous la barre du sondage SD à 11 s ; le
second est la mesure fidèle à ce que verrait un utilisateur sur un vrai
terminal.
