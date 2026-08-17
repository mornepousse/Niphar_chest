# Carte-clé WT9932 — spécification de conception

*2026-08-16*

## Problème

Le projet a deux cartes : le kit de dev, où la confirmation vient d'une commande
console sans aucune valeur de sécurité, et le coffre, où elle viendra du clavier
par un lien SPI qui n'existe pas encore. Aucune des deux ne permet aujourd'hui
d'éprouver la chaîne complète — *l'hôte demande une signature, un humain touche
un contact, l'opération passe* — parce qu'aucune n'a de contact.

La carte WT9932P4-TINY change ça. Elle n'est ni un kit ni un coffre : c'est une
**troisième variante de produit**, une clé de sécurité autonome, qui fait avec
ses propres boutons ce que la variante intégrée fera avec le clavier. Les deux
sont des produits ; ni l'une ni l'autre n'est la béquille de l'autre.

Cette spec couvre la carte, son IHM locale, et le remaniement de l'abstraction
de carte que sa seule existence rend nécessaire.

## Portée

**Dans la portée** : le fichier de carte, un sous-système IHM (deux boutons, une
LED adressable), la séparation des drapeaux de carte, la désactivation de la
microSD, et le branchement du bouton de confirmation sur `sec_confirm`.

**Hors portée** : le lien S3 (tâche #9, inchangée), FIDO/CTAP, et toute
modification de la pile OpenPGP ou OTP — cette carte les exécute telles quelles.

## Matériel — ce qui est établi et par quoi

Carte **WT9932P4-TINY_1v2**, module **WT0132P4-A1** (ESP32-P4 rev v1.0, 32 Mo
PSRAM, 16 Mo flash). Source : le schéma constructeur `Schematic-ESP32P4-TINY-
WT0132P4-A1-Pocket-Development-Board`, JLCEDA V1.0, révisé 2025-08-07.

| élément | fait | source |
|---|---|---|
| LED adressable | `DIN ← IO51`, `VDD ← 5 V`, découplage 100 nF | schéma, bloc LED |
| LED témoin | LED simple sur R13 1 kΩ, non pilotable | schéma, bloc LED |
| Bouton BOOT | SW2 → **IO35**, R4 10 kΩ vers 3,3 V, C8 100 nF | schéma, bloc KEY |
| Bouton RESET | SW1 → CHIP_PU, R1 10 kΩ, C1 100 nF | schéma, bloc KEY |
| USB OTG HS | J4 → self L3 → `USB_DP`/`USB_DM` (broches PHY dédiées) | schéma, bloc USB |
| USB-Serial-JTAG | J3 → self L2 → `IO25`/`IO24` | schéma, bloc USB串口 |
| microSD | **absente** — aucun connecteur au schéma | schéma, page 1/2 |
| Broches libres | IO26–IO33 sorties sur J7 | schéma, connecteur J7 |

Vérifié sur le matériel le 2026-08-16 : PSRAM 32 Mo à 200 MHz en mode X16
(`esp_psram: SPI SRAM memory test OK`, 32 320 K au tas), `gpg --card-status`
répond sur le port OTG, et le cycle `none → pgp → otp → storage → none` passe.

### IO35 n'est pas utilisé, et c'est délibéré

IO35 est le pin de strapping du mode de boot du P4 — l'équivalent de l'IO0 des
ESP32-S3, et le constructeur y a placé le bouton BOOT pour cette raison.

> « ESP32-P4 has five strapping pins: GPIO34, GPIO35, GPIO36, GPIO37, GPIO38 »
> — *ESP32-P4 TRM*, ch. 11 « Chip Boot Control », p. 795
>
> Table 11.2-2 — `SPI Boot mode (default) : GPIO35 = 1` ·
> `Joint Download Boot mode : GPIO35 = 0, GPIO36 = 1`
> — *idem*, p. 796

Le silicium autorise pourtant son usage : « After the reset is released, the
strapping pins work as normal-function pins » (*idem*, §11.2.1). On s'en prive
quand même, pour trois raisons cumulées :

1. **Un appui pendant la mise sous tension empêche la clé de démarrer** — elle
   part en mode download. Un reset accidenté bouton enfoncé (brownout, chien de
   garde) fait de même, et la clé disparaît du bus sans explication.
2. **Le garde-fou n°1 de `scripts/fast.sh` resterait vert.** Il exclut déjà
   `boards/*/board.h` de son grep — « ce sont eux qui les déclarent réservés ».
   Déclarer `BOARD_BUTTON GPIO_NUM_35` y passerait donc sans bruit, alors que le
   sens de GPIO35 s'y inverserait : de *réservé* à *bouton utilisateur*. Le garde
   surveille le nom, pas l'intention.
3. **La marge d'amorçage est étroite.** C8 (100 nF) charge à travers R4 (10 kΩ),
   soit τ ≈ 1 ms sur un pin échantillonné au reset. Ce qui sauve la carte est le
   RC identique sur CHIP_PU, qui retient la puce le temps que IO35 monte. Ça
   fonctionne — vérifié une dizaine de fois — mais c'est un appariement de
   constantes, pas une garantie.

Les boutons vont donc sur **IO32** et **IO33**, câblés par l'utilisateur vers la
masse, pull-up interne activé côté firmware. Ce sont, avec leurs voisins IO26–31,
les seules broches du P4 dont les trois colonnes de la table GPIO sont vides :
ni fonction analogique, ni LP GPIO, ni restriction.

> `GPIO26 | | |` … `GPIO33 | | |`
> — *ESP-IDF Programming Guide*, « GPIO & RTC GPIO — ESP32-P4 », § GPIO Summary

Elles sont aussi hors des deux blocs occupés chez les cartes sœurs — SD sur
39–48, lien S3 sur 7–11 — donc sans collision conceptuelle.

## Architecture

### 1. Séparer ce que `BOARD_LINK_AVAILABLE` confondait

Ce drapeau répond aujourd'hui à deux questions à la fois : « y a-t-il un lien
SPI ? » et « la béquille console est-elle permise ? ». La fusion tenait tant
qu'il n'y avait que deux cartes. La troisième la casse : pas de lien, un bouton,
et la console conservée.

```c
/* main/board_common.h */
#define BOARD_CONFIRM_NONE    0   /* aucune source réelle de présence */
#define BOARD_CONFIRM_LINK    1   /* le S3, par le lien SPI */
#define BOARD_CONFIRM_BUTTON  2   /* un bouton en façade */
```

Trois questions, trois drapeaux :

| carte | `BOARD_CONFIRM_SOURCE` | `BOARD_LINK_AVAILABLE` | `BOARD_CONSOLE_ACTIONS` | `BOARD_HAS_SD` |
|---|---|---|---|---|
| `jc_devkit` | `NONE` | 0 | 1 | 1 |
| `niphar_chest` | `LINK` | 1 | **0** | 1 |
| `wt9932_key` | `BUTTON` | 0 | 1 | **0** |

`BOARD_LINK_AVAILABLE` retrouve un sens unique et littéral. Le garde-fou n°4 de
`fast.sh` cesse de s'y référer et s'appuie sur `BOARD_CONSOLE_ACTIONS`, qui est
la question qu'il pose réellement. Trois `_Static_assert` dans `board_common.h`
verrouillent la cohérence :

```c
_Static_assert(BOARD_CONFIRM_SOURCE == BOARD_CONFIRM_NONE
            || BOARD_CONFIRM_SOURCE == BOARD_CONFIRM_LINK
            || BOARD_CONFIRM_SOURCE == BOARD_CONFIRM_BUTTON,
    "BOARD_CONFIRM_SOURCE n'a pas une des trois valeurs connues");

#if BOARD_CONFIRM_SOURCE == BOARD_CONFIRM_LINK
_Static_assert(BOARD_LINK_AVAILABLE,
    "présence annoncée par le lien sur une carte qui n'en a pas");
#endif

/* `defined()` n'existe pas dans une expression C : ce troisième contrôle se
 * fait au préprocesseur, pas en _Static_assert. */
#if BOARD_CONFIRM_SOURCE != BOARD_CONFIRM_BUTTON && defined(BOARD_BTN_CONFIRM)
#error "bouton de confirmation déclaré sur une carte dont ce n'est pas la source"
#endif
```

### 2. `BOARD_HAS_SD` — sans quoi la clé met onze secondes à démarrer

`board_common.h` définit aujourd'hui le brochage SD inconditionnellement et
`main.c:58-62` sonde au démarrage. Sur une carte sans connecteur, le sondage
échoue par expiration, deux fois (chemin externe puis LDO), et coûte **environ
onze secondes** — mesuré sur ce module le 2026-08-16 :

```
E (1861)  sdmmc_periph: sdmmc_host_clock_update_command … returned 0x107
E (10861) sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned 0x107
W (10891) sd: aucune carte (ESP_ERR_TIMEOUT) — le coffre reste utilisable, la SD non
```

Onze secondes avant qu'une clé de sécurité ne réponde à son premier appui, pour
chercher un composant qui n'est pas soudé. `BOARD_HAS_SD 0` supprime le brochage,
l'appel, et le mode `storage` du cycle.

Corollaire : les blocs SD de `board_common.h` passent sous `#if BOARD_HAS_SD`, et
`console.c` conditionne la commande `sd`. `sd_card.c` n'est pas modifié — il
n'est simplement plus appelé.

### 3. Sous-système `main/hmi/`

Découpage selon la norme TDD du projet : ce qui calcule est pur et testé sur
l'hôte, ce qui touche au matériel est mince et non testé.

| fichier | nature | responsabilité |
|---|---|---|
| `hmi/button_debounce.h` | **pur** | filtre un niveau brut en fronts stables |
| `hmi/led_state.h` | **pur** | `(mode, attente, verdict) → (couleur, couleur alt, motif)` |
| `usb/usb_mode_cycle.h` | **pur** | `none → pgp`, puis `pgp ⇄ otp` |
| `hmi/hmi.c` | matériel | GPIO d'entrée, `led_strip` en RMT, la tâche |

**Deux boutons, un métier chacun.** IO32 bascule le mode, IO33 confirme. Aucun
seuil de durée, donc aucune ambiguïté : un appui est une action, et rien d'autre.
C'est la raison de fond du choix, pas un confort. Avec un bouton unique
distinguant appui long et appui court, le seuil temporel serait la seule chose
séparant « je confirme cette signature » de « je désinstalle le CCID pendant que
l'hôte s'en sert » — un doigt qui traîne arracherait l'interface en pleine
opération. Une porte de présence physique doit faire une chose.

**L'appui long n'existe plus, et `usb_mode_set()` reste confiné.** Le garde-fou
n°4 restreint ce symbole à `usb_mode.c` et `console.c` ; l'élargir à `hmi.c`
affaiblirait le garde pour un gain nul. `usb_mode.c` expose donc
`usb_mode_cycle_next()`, et la politique du cycle reste chez le module qui
possède les modes.

### 4. `sec_confirm_peek()` — sans quoi la LED vole les signatures

La LED doit signaler quand une opération est armée, donc la tâche IHM doit
connaître l'état de `sec_confirm`. Or `sec_confirm_poll()` **consomme** la
permission :

> « AUTHORIZED -> writes slot to *out_slot, consumes (-> IDLE), returns AUTHORIZED »
> — `main/security/sec_confirm.h:20-21`

Une tâche d'affichage qui appellerait `poll()` volerait la permission à celui qui
l'attend, et la signature échouerait sans trace. Il faut donc une lecture sans
effet de bord :

```c
/* Lit l'état sans rien consommer ni expirer. Pour l'affichage seulement :
 * seul poll() fait avancer la machine. */
sec_confirm_state_t sec_confirm_peek(uint32_t now_ms);
```

Pure, sans état muté, testable avec le reste de `sec_confirm`. `now_ms` sert à
signaler une expiration déjà atteinte sans la consommer — la LED doit pouvoir
montrer le refus.

## Comportement

```
branchement ──▶ [none]  LED éteinte, rien sur le bus
                   │
                   │ appui MODE (IO32)
                   ▼
              [pgp] ●bleu ◀── appui MODE ──▶ [otp] ●vert

opération armée      ●bleu/●rouge (ou ●vert/●rouge en otp) — alternance
                       pleine luminosité, 1 Hz, franche (pas de fondu)
appui CONFIRM (IO33) ☀ flash blanc 120 ms — SEULEMENT si une opération
                       était armée → sec_confirm_authorize()
refus / expiration   ☀ flash rouge 120 ms
bascule de mode      ☀ flash de la nouvelle couleur, 120 ms
```

**Révision 2026-08-17, après usage réel de la carte.** L'attente de
confirmation pulsait à l'origine en luminosité (0 → `LED_DIM` sur la couleur du
mode). Éprouvée sur matériel, cette pulsation s'est révélée trop discrète : on
la rate si on ne fixe pas la LED, et une porte de présence physique qu'on ne
voit pas s'ouvrir fait rater des signatures. L'attente alterne désormais
franchement entre la couleur du mode et le rouge, toutes deux à `LED_BRIGHT` —
`led_state_view()` expose ce second terme via `rgb_alt`, et `hmi.c` bascule
entre `rgb` et `rgb_alt` sans jamais connaître leur signification.

**Réserve assumée.** Le rouge porte maintenant deux sens opposés — « refusé »
sur le flash de 120 ms, « en attente » sur l'alternance 1 Hz. Ce n'est pas une
ambiguïté fortuite : c'est un compromis délibéré, où la durée est le seul signe
distinctif (120 ms contre 15 s, `SEC_CONFIRM_TIMEOUT_MS`). Retenu malgré la
mise en garde, parce que rater une fenêtre de confirmation coûte plus cher
qu'une seconde de lecture attentive sur la durée du signal.

Conforme au principe du projet : **rien n'est exposé au démarrage**. La clé
arrive muette et le premier appui MODE l'arme. `none` n'est plus atteint ensuite
sans débrancher — décision assumée : deux crans valent mieux que trois à l'usage,
et débrancher une clé est un geste naturel.

Anti-rebond logiciel de 20 ms, sur front descendant (boutons actifs bas). Les
boutons de l'utilisateur n'ont pas de RC : le filtrage est entièrement logiciel,
contrairement à SW2 qui en a un.

Luminosité basse au repos — 20/255 sur les trois canaux — et 120/255 sur les
flashes de verdict et sur l'alternance d'attente, qui doivent se voir. C'est
une clé, pas une lampe.

## Gestion des absences

| situation | comportement |
|---|---|
| `usb_mode_cycle_next()` échoue | mode conservé, `known = false` (mécanique existante), flash rouge |
| appui CONFIRM hors opération armée | `sec_confirm_authorize()` est déjà un no-op ; aucun flash, pour ne pas suggérer qu'il s'est passé quelque chose |
| LED absente ou muette | `hmi.c` journalise et continue ; l'absence d'affichage ne bloque jamais une opération |
| rebond sur MODE pendant une signature | le mode bascule, le CCID est désinstallé — c'est le comportement demandé, et l'anti-rebond en est la seule protection |

## Vérification

**Sur l'hôte, sans matériel**, tests écrits avant l'implémentation :

- `test_button_debounce.c` — rebond au front, appuis enchaînés, maintien long
  (qui ne doit produire **qu'un** front), relâchement pendant le rebond.
- `test_led_state.c` — totalité du mapping : aucun état sans couleur, aucune
  couleur partagée par deux modes, l'alternance n'est produite que sur attente,
  ses deux couleurs diffèrent et sont visibles, `rgb_alt` vaut `rgb` hors
  attente.
- `test_usb_mode_cycle.c` — `none → pgp`, `pgp → otp`, `otp → pgp`, et `none`
  jamais rendu après le premier appel.
- `test_sec_confirm.c` (existant, étendu) — `peek()` ne consomme pas : un `peek`
  suivi d'un `poll` rend toujours `AUTHORIZED`.

Chaque test doit mordre : bug transitoire introduit, rouge constaté, retour.

**Sur la carte** :

- démarrage muet, aucun périphérique USB, LED éteinte ;
- appui MODE → CCID énumère, `gpg --card-status` répond, LED bleue ;
- `gpg --card-status` puis une signature → alternance bleu/rouge, appui
  CONFIRM, flash blanc, signature produite ;
- ne pas appuyer → flash rouge à 15 s (`SEC_CONFIRM_TIMEOUT_MS`), `gpg` échoue ;
- appui MODE → HID énumère, LED verte ;
- temps du démarrage à la LED : **inférieur à une seconde** (contrôle du
  `BOARD_HAS_SD`).

**Sur les cartes sœurs**, non-régression : `jc_devkit` et `niphar_chest`
construisent et se comportent comme avant. `sec_gate_console_confirm` reste
absent du binaire `niphar_chest` (contrôle `nm` existant).

## Ce que cette carte ne prouvera pas

Il faut l'écrire, sinon on se persuadera d'avoir validé plus que ça.

- **Le lien S3** : absent des trois cartes. La variante intégrée reste non
  éprouvée.
- **L'échange CR-HMAC** : pas d'outillage HID sur la machine de développement.
  Le mode OTP n'est vérifié que jusqu'à la liaison par le noyau.
- **La résistance au dump de flash** : la carte a un bouton BOOT en façade et un
  bouton RESET. N'importe qui, avec la carte en main, entre en mode download et
  lit la flash — qui contient les clés privées OpenPGP. La parade prévue pour la
  carte de production (jumpers JTAG retirés en fabrication, cf.
  `docs/HARDWARE.md`) ne s'applique pas ici.
- **Les contraintes d'irrécupérabilité du coffre** : cette carte pardonne, comme
  le kit. Elle éprouve un *comportement* de sécurité, pas une posture matérielle.

## Risques

| risque | portée | traitement |
|---|---|---|
| LED en 5 V pilotée par une donnée 3,3 V | un WS2812 strict exige VIH ≥ 0,7 × VDD = 3,5 V ; on est dessous | à constater au banc. Couleurs fausses ou scintillement = cause matérielle, pas logicielle. Parade si besoin : alimenter la LED en 3,3 V |
| Broches lues sur un rendu du schéma | J7 et le brochage des boutons | à recouper avec la sérigraphie avant de souder |
| `BOARD_HAS_SD` traverse `board_common.h` | touche les trois cartes | les deux cartes existantes gardent `1` ; non-régression couverte par `check.sh` complet |
| Le remaniement des drapeaux touche `fast.sh` | garde-fou n°4 | garde réécrit sur `BOARD_CONSOLE_ACTIONS`, et son efficacité re-prouvée par mutation |

## Divergences à déclarer

À inscrire dans `.tripwire-divergences` au moment de l'introduction :

1. `fast.sh` — garde-fou n°4 sur `BOARD_CONSOLE_ACTIONS` et non plus
   `BOARD_LINK_AVAILABLE` : le motif surveillé change de nom.
2. `board_common.h` — le brochage SD devient conditionnel ; le motif
   `BOARD_SD_CLK` n'est plus inconditionnellement présent.
