# Lien S3↔coffre — SPI, le coffre en esclave

Design validé le 2026-08-07. Deuxième incrément du firmware du coffre.

## 1. Problème

Le coffre n'a **aucune source de présence physique**. Pas de bouton, pas de
contact, aucune liaison avec le clavier : le seul canal qui lui parle est l'USB,
c'est-à-dire précisément l'adversaire du modèle de menace.

Or la solidité de la pile OpenPGP de `KeSp_firmware` tient à un unique verrou :
`sec_confirm` — un appui réel sur une touche, qu'un malware hôte ne peut pas
fabriquer. Le PIN, lui, se capture. Sans lien, un OpenPGP sur le coffre serait
protégé par le seul PIN, strictement plus faible que le dongle existant, et un
FIDO conforme serait impossible : CTAP exige un test de présence utilisateur.

S'y ajoute un besoin fonctionnel : pouvoir **piloter les options du coffre depuis
le clavier** — sélection d'image bootable, activation de fonctions — sans passer
par l'hôte.

Le lien apporte les deux à la fois.

## 2. Portée

Dans la portée : le transport SPI, le protocole et son versionnement, la
détection d'absence, et un seul message — « l'utilisateur a confirmé ».

Hors portée, chacun avec sa propre spec : le pilotage des options, le portage
OpenPGP CCID, FIDO/CTAP, et le côté S3 dans `KeSp_firmware`.

## 3. Brochage

| signal | coffre (P4) | clavier (S3) |
|---|---|---|
| CS | GPIO7 | GPIO3 |
| MOSI | GPIO8 | MOSI SPI2 existant |
| SCK | GPIO9 | SCK SPI2 existant |
| MISO | GPIO10 | MISO SPI2 existant |
| IRQ (coffre→S3) | GPIO11 | GPIO46 |

Côté coffre, c'est le quatuor IOMUX natif de SPI2 (`spi_slave.rst:157-162`,
valeurs `esp32p4`), donc chemin direct. Ça n'est pas cosmétique : le driver
rebascule **tout** le bus sur la matrice GPIO dès qu'un seul signal est hors
IOMUX, et la matrice allonge le retard d'entrée de MISO — sur un faisceau entre
deux cartes, cette marge compte.

Côté clavier, le coffre est un troisième client du bus SPI2 déjà partagé entre le
NRF24 et l'e-ink (`KeSp_firmware`, `boards/kase_half_left/board.h:52-58,78-79`),
d'où un coût d'un seul pin : le CS.

### Trois contraintes non négociables

1. **Mode 0** (CPOL=0, CPHA=0). Pas une préférence : c'est ce qui garde SCK au
   repos à l'état bas, condition qui rend déjà sûr le strap `GPIO_NUM_45` côté
   S3 dans le code existant.
2. **GPIO35 du P4 interdit.** C'est le seul strap qui choisit entre boot
   applicatif et mode download (TRM table 11.2-2). Le coffre n'ayant pas de
   bouton de secours, s'y tromper coûte un fer à souder.
3. **IRQ active à l'état haut, pull-down côté S3.** Le coffre ne vit qu'en
   filaire alors que la moitié gauche tourne sur batterie : il est **non
   alimenté la plupart du temps**, et son GPIO11 est alors en haute impédance. Un
   pull-up injecterait du courant dans un rail éteint à travers les diodes de
   protection du P4. Avec un pull-down, coffre absent = ligne au repos, rien ne
   circule.

### L'IRQ arrive sur un pin de strapping du S3, sans conséquence

`GPIO0, GPIO3, GPIO45 et GPIO46` sont les pins de strapping de l'ESP32-S3
(`esp-idf/docs/en/api-reference/peripherals/gpio/esp32s3.inc:252`), et l'IRQ
arrive sur GPIO46. Ça mérite un examen, pas une inquiétude :

- **GPIO46** ne contrôle que l'impression des messages ROM sur UART0. Avec
  l'eFuse `EFUSE_UART_PRINT_CONTROL` à sa valeur par défaut, son niveau au reset
  est explicitement marqué « Ignored » — ESP32-S3 TRM v1.8, table 8.3-1, p. 536.
- **GPIO45**, en revanche, choisit la tension du rail flash (bas → 3,3 V,
  haut → 1,8 V). Y faire arriver une IRQ active à l'état haut aurait pu empêcher
  le clavier de démarrer. Ce pin est de toute façon déjà `BOARD_NRF_SPI_SCK` sur
  `kase_half_left`.

**Invariant firmware conservé malgré tout :** le coffre n'asserte jamais IO11
tant que le S3 ne lui a pas parlé au moins une fois. Ça ne coûte rien, ça garde
la ligne calme pendant le boot du clavier, et ça protège d'un changement futur
de cet eFuse. Ce n'est en revanche plus une règle de sécurité au sens de
l'interdiction de GPIO35 : la dégrader ne casserait plus rien.

## 4. Architecture

```
main/link/
├── link_proto.{c,h}   logique pure : carte de registres, trames, CRC
├── link_spi.{c,h}     transport : spi_slave_hd, ligne IRQ
└── sec_confirm.{c,h}  porté de KeSp_firmware, inchangé
```

**`link_proto`** ne contient aucun appel ESP-IDF et compile sur l'hôte. C'est là
que vit tout ce qui peut être faux sans être visible : sérialisation de la carte
de registres, détection d'absence, cadrage et CRC des trames, comparaison de
version. C'est aussi, et ce n'est pas un hasard, **la première logique pure du
projet** — donc le premier morceau soumis à la norme TDD du `CLAUDE.md`.

**`link_spi`** ne fait que du transport : `spi_slave_hd` sur le brochage de
`board.h`, publication des registres, pilotage de la ligne IRQ. Il ne décide de
rien.

**`sec_confirm`** est repris tel quel de `KeSp_firmware/main/security/` —
module pur, sans NVS ni matériel, déjà couvert par des tests hôte qu'on porte
avec lui. Le réécrire serait une régression de sécurité pour rien.

### Deux étages, délibérément

**Registres partagés** — lisibles et écrivables par le maître *sans que le
firmware du coffre ait rien préparé*, puisque c'est le matériel qui répond.
C'est ce qui rend le diagnostic possible même coffre planté, et c'est la raison
du choix de `spi_slave_hd` plutôt que de l'esclave classique : ce dernier exige
que l'esclave ait posté un tampon avant chaque transaction, ce qui demanderait
une ligne « ready » qu'on n'a pas.

| offset | champ | sens |
|---|---|---|
| 0x00-0x03 | mot magique `NIPH` | coffre→S3, présence |
| 0x04 | version du protocole | coffre→S3 |
| 0x05 | état (bits : carte SD présente, USB monté, prêt) | coffre→S3 |
| 0x06-0x07 | code de l'opération en attente de confirmation | coffre→S3 |
| 0x08-0x0B | compteur de confirmations consommées | coffre→S3 |
| 0x0C | confirmation utilisateur | S3→coffre |

**Canal de données** pour les messages plus longs — spécifié ici, **non
implémenté dans cet incrément**. Il portera le pilotage des options.

### Format de trame (canal de données)

```
[0]    SOF 0xA5
[1]    version
[2]    opcode
[3]    longueur de charge utile (0-247)
[4..]  charge utile
[n-2]  CRC16, petit-boutiste
```

Le SPI ne fournit ni cadrage ni détection d'erreur : les deux sont à la charge du
protocole. La version est dans **chaque** trame, pas seulement dans les
registres : les deux dépôts ne seront pas toujours flashés ensemble, et une
version inattendue doit faire rejeter la trame plutôt que l'interpréter de
travers.

## 5. L'absence est le cas normal

Le coffre n'existe que branché en USB ; la moitié gauche vit surtout sur
batterie. **Coffre absent n'est pas une erreur** : le S3 ne doit ni attendre, ni
journaliser, ni consommer pour ça.

Un MISO flottant se lit `0x00` ou `0xFF` selon la terminaison. Le mot magique
distingue ces deux cas d'un vrai coffre, et c'est précisément le genre de
détail qu'on croit évident et qu'on code à l'envers — d'où des tests dédiés.

## 6. Gestion des erreurs

- Version de protocole inconnue : trame rejetée, compteur incrémenté, aucun
  effet de bord. Jamais d'interprétation partielle.
- CRC faux ou longueur incohérente : rejet silencieux. Le SPI n'a pas d'accusé
  de réception ; le maître réessaiera s'il lui importe.
- Confirmation reçue sans opération armée : sans effet, comme
  `sec_confirm_authorize()` hors état `PENDING`. Ce n'est pas une erreur, c'est
  un appui hors contexte.
- Échec d'init du lien : journalisé, le coffre continue. Le lien est un
  supplément ; le MSC et la console doivent survivre à son absence.

## 7. L'invariant de sécurité

Un seul, et il vit dans `KeSp_firmware` : **le S3 n'écrit une confirmation que
sur appui réel de la touche.** Jamais depuis le protocole CDC, qui parle à
l'hôte — sinon on rouvre à l'hôte le seul verrou qu'il ne devait pas pouvoir
franchir, et tout l'intérêt du lien disparaît.

Cet invariant est hors du présent dépôt mais fait partie de cette conception. Il
mérite une règle `check.sh` côté KeSp au moment d'y écrire le pendant.

## 8. Vérification

Sur l'hôte, sans matériel :

- cadrage des trames, longueurs incohérentes, CRC faux, trame tronquée ;
- rejet d'une version inconnue ;
- détection d'absence sur `0x00` et sur `0xFF` ;
- aller-retour de la carte de registres ;
- comportement de `sec_confirm` (tests portés depuis KeSp).

Sur le kit de dev, ce qui reste vérifiable : le build `jc_devkit` passe avec le
lien désactivé, et **rien de ce qui marchait ne régresse** — la microSD, le MSC,
et les débits mesurés au socle (9,4 et 5,3 Mio/s).

**Ce qui n'est pas validable, et c'est nouveau.** Sur le kit, GPIO7-11 sont pris
par le codec audio et son I2C. Ce lien est donc le premier morceau du projet
écrit sans pouvoir être prouvé au banc — tout le reste l'a été dans la foulée.
Reste en dette jusqu'à la carte révisée : l'énumération réelle, la latence de
l'IRQ, le branchement et le débranchement à chaud, et surtout la fenêtre de
strapping `VDD_SPI` — le seul point où une erreur logicielle empêcherait le
**clavier** de démarrer.

## 9. Décisions et alternatives écartées

| Décision | Alternative écartée | Raison |
|---|---|---|
| SPI, coffre esclave | I2C sur le bus existant du S3 | l'I2C ne coûtait aucun pin côté S3, mais un coffre qui bloque le bus (clock stretching, plantage) figerait aussi l'écran du clavier |
| `spi_slave_hd` | `spi_slave` classique | l'esclave classique exige un tampon posté avant chaque transaction ; sans ligne « ready », un coffre occupé à servir le MSC perdrait des transactions |
| Confirmation poussée par l'utilisateur | le S3 interroge en boucle et affiche l'invite | le polling permanent coûte de la batterie sur une moitié qui en vit, alors que le coffre est absent la plupart du temps. L'invite sur l'e-ink redeviendra possible via la ligne IRQ, dans un incrément ultérieur |
| `sec_confirm` porté sans modification | réécriture adaptée au coffre | module pur, déjà validé et testé sur matériel ; le réécrire serait une régression de sécurité gratuite |
