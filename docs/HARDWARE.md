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
- **Aucune ligne d'interruption** : les deux côtés sont pleins. Le coffre ne
  peut jamais prendre la parole, seul le S3 initie.

Sur le kit de dev, ces quatre pins sont pris (I2C du codec sur 7/8, I2S sur
9/10) : le prototype devra câbler ailleurs, via la matrice GPIO. C'est la
première divergence réelle de brochage entre les deux cartes.

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
tiennent par construction — `_Static_assert` dans `main/board.h` et greps dans
`scripts/fast.sh` — et ces garde-fous ne doivent pas être contournés « juste
pour un test ».

## Divers

- **C6 embarqué du module : vierge et non câblé** (U0RXD/U0TXD/IO9 NC) — flashable
  uniquement via le P4 (esp-hosted / OTA). IO9 flottant = boot normal, sûr.
- Straps P4 (GPIO34-38) : tous NC ; GPIO35 a un pull-up interne → SPI boot par défaut.
- 9 pins GND du module câblés ; DSI/CSI/LDO_VO4 non câblés (assumé).
- Lien direct S3↔P4 : AUCUN (abandonné) — le P4 parle à l'hôte par l'USB, point.
