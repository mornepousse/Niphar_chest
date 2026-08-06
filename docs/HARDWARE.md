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

### ⚠ Point ouvert : VDDPST_5 (SD_VREF)

Le contrat ci-dessus ne dit rien de l'alimentation des IO de la carte. Or sur
ESP32-P4 c'est elle qui décide si la microSD fonctionne :

> « ESP32-P4 SDMMC Host requires the IO voltage to be supplied externally via
> the **VDDPST_5 (SD_VREF)** pin. If the design doesn't require the higher speed
> SD modes, this pin can be simply connected to the 3.3V supply. »
> — ESP-IDF Programming Guide, *SDMMC Host Driver* (ESP32-P4),
> § Configuring Voltage Level.

Le coffre étant en IO fixe 3,3 V sans SDR104, `LDO_VO4 non câblé` est cohérent —
**à condition que VDDPST_5 soit bien relié au rail 3,3 V**. À vérifier sur la
netlist avant de déclarer le contrat complet. Si le pin est laissé flottant,
aucune carte ne répondra, et aucun firmware n'y pourra rien.

Le firmware sonde les deux chemins au démarrage (alimentation externe, puis LDO
interne canal 4) et journalise celui qui a fonctionné : le premier bring-up avec
carte tranchera la question.

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
