# Niphar_chest — Claude Code instructions

Firmware ESP32-P4 du **coffre** du clavier split
[Niphargus](https://github.com/mornepousse/Niphargus) : un P4 embarqué dans la
moitié gauche, derrière un hub USB, qui ne s'éveille qu'en filaire. Build via
ESP-IDF 5.5.

## Repo

- **Origin** : https://github.com/mornepousse/Niphar_chest
- **Local** : `~/Documents/GitHub/Niphar_chest/`
- **Voisins** : [KeSp_firmware](https://gitlab.com/harrael/KeSp_firmware) — pile
  OpenPGP CCID déjà validée sur matériel (`main/security/`), à reprendre pour le
  volet PGP/FIDO ; specs dans ses `docs/OPENPGP_CARD.md` et `docs/SECURITY_KEY.md`.

## Contraintes matérielles irréversibles

Contrat complet : [`docs/HARDWARE.md`](docs/HARDWARE.md), vérifié à la netlist.
Deux règles priment sur tout le reste :

1. **Ne jamais réaffecter GPIO24/25.** C'est l'USB-Serial-JTAG, et c'est le seul
   chemin de flash et de debug du coffre : pas de bouton reset, pas d'accès
   matériel au mode download. Un firmware qui casse ce lien se répare au fer à
   souder.
2. **Ne jamais entrer en deep-sleep permanent**, pour la même raison.

`main/board_common.h` porte des `_Static_assert`, et `scripts/fast.sh` des greps, qui
font échouer le build sur ces deux points. Ne pas les contourner.

**Le kit de dev pardonne, le coffre non.** Le JC-ESP32P4-M3-DEV a un CH340C et
un bouton BOOTMODE ; ces règles n'y seront donc jamais vérifiées à l'exécution.
Elles tiennent par construction, pas par expérience.

## Le coffre n'expose rien au démarrage

À froid, le coffre démarre en `USB_MODE_NONE` (`main/usb/usb_mode.h`) :
**aucune** interface USB fonctionnelle n'est installée — ni disque, ni carte à
puce, ni HID. C'est délibéré (« plein de choses, une à la fois », jamais deux
en même temps) et **c'est le comportement normal**, pas une panne : un coffre
qui vient d'être flashé ou reseté n'apparaîtra dans aucun `lsusb`/`lsblk`/
`gpg --card-status` tant qu'on ne lui a rien demandé.

Le sélecteur est la console série (`main/console/console.c`), pas l'USB
lui-même :

```
usb mode none       # rien exposé — l'état de repos
usb mode storage     # microSD en MSC
usb mode pgp          # carte OpenPGP en CCID
usb mode otp          # clé CR-HMAC en HID
usb mode fido          # authentificateur U2F/CTAP-HID
```

Chaque bascule désinstalle d'abord le mode courant (`usb_device_uninstall()`
— vraie déconnexion USB vue par l'hôte) avant d'installer le suivant : à tout
instant, au plus un jeu de descripteurs est présent. Voir `main/usb/usb_mode.c`.

**Effet de bord à connaître** : `usb mode pgp` recharge l'état persistant
OpenPGP (DO, PIN, clés — `usb/mode_pgp.c:mode_pgp_data_load()`) à **chaque**
entrée dans le mode, pas seulement au premier boot — nécessaire parce que
`ccid_drv_init()` réarme les PIN d'usine en RAM à chaque bascule. Charger cet
état au démarrage (comme `sd_probe()`/`sec_gate_init()`) aurait été plus
simple mais contredit ce principe : mettre des clés privées en RAM avant que
quiconque n'ait demandé le mode PGP n'a pas de sens. Détail et preuve sur
matériel : [`docs/HARDWARE.md`](docs/HARDWARE.md#validation-openpgp-ccid--2026-08-07).

## Build

Trois cartes. `jc_devkit` et `niphar_chest` ne divergent que sur le lien
S3↔coffre, absent du kit ; tout le reste (microSD, USB) est commun et vit dans
`main/board_common.h`. `wt9932_key` est la troisième — la clé de sécurité
autonome (WT9932P4-TINY), sans lien S3 ni microSD, avec boutons et LED en
façade — voir [`docs/HARDWARE.md`](docs/HARDWARE.md) pour son brochage.

| carte | lien S3 | matériel |
|---|---|---|
| `jc_devkit` | non | le kit, premier matériel qui a existé |
| `niphar_chest` | oui | le coffre, pas encore fabriqué |
| `wt9932_key` *(variant courant, `.tripwire-variant`)* | non | la clé autonome, seul matériel réellement flashé au quotidien |

```bash
source ~/esp/esp-idf/export.sh
idf.py -B build_wt9932_key -DBOARD=wt9932_key -DSDKCONFIG=build_wt9932_key/sdkconfig build
```

`.tripwire-variant` (committé, lu par `.esp-dev.yml`) porte le nom de la carte
que `/esp-build`/`/esp-flash`/`/esp-cycle` construisent et flashent par
défaut — **vérifier sa valeur avant de flasher** : un défaut périmé y a déjà
fait flasher le firmware `jc_devkit` (sans écran) sur la carte-clé, un
incident documenté dans `docs/HARDWARE.md`. Le sens de l'erreur reste
asymétrique entre cartes à lien S3 : flasher du `jc_devkit` sur un coffre ne
fait que priver du lien, l'inverse enverrait du SPI dans le bus I2C du codec
audio du kit — `wt9932_key` n'a ni l'un ni l'autre bus, donc n'est concerné
que par la première règle générale (jamais GPIO24/25, jamais de deep-sleep
permanent).

Aucun source n'inclut un chemin de carte : `${BOARD_DIR}` est en tête des
includes, donc `#include "board.h"` résout vers la carte sélectionnée.

Flash et monitor : `/esp-build`, `/esp-flash`, `/esp-cycle`, `/esp-monitor`
(config dans `.esp-dev.yml`).

**Port série — attention.** Ne jamais élargir le glob à `/dev/ttyACM*` : sur
cette machine, `/dev/ttyACM0` est le clavier KaSe V2 Debug (`cafe:4001`). Le
port du P4 se désigne par
`/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*-if00`.

`idf.py monitor` exige un vrai TTY et ne marche donc pas depuis un agent. Pour
capturer un boot sans TTY : reset en pulsant RTS seul (DTR bas straperait la
puce en mode download) puis lire le port.

## Versioning

Source de vérité : le tag git `vX.Y.Z`, lu au build par `git describe --tags`.
Pas de fichier VERSION. La macro `NIPHAR_VERSION` est appliquée au seul
composant `main` : la passer en `add_compile_definitions()` global ferait
recompiler tout le projet à chaque commit.

## Workflow anti-régression (OBLIGATOIRE)

Source unique de vérité : `scripts/check.sh`.
- `./scripts/check.sh --fast` — garde-fous matériels du coffre + build ESP-IDF incrémental (~secondes)
- `./scripts/check.sh` — fast + le rebuild complet depuis zéro

`check.sh` n'exécute pas ces phases lui-même : il appelle `scripts/fast.sh` et
`scripts/full.sh`. C'est là qu'on ajoute un garde-fou ou une suite de tests —
`check.sh` est un fichier templaté que les mises à jour de tripwire réécrivent.

**Activation des hooks git (une fois par clone)** :
```bash
./scripts/install-hooks.sh   # ou: git config core.hooksPath scripts/hooks
```
`pre-push` lance le check complet et bloque le push si rouge. WIP : `git push --no-verify`.

**Hooks Claude Code** (`.claude/settings.json`, automatiques) :
- `PostToolUse` sur édition d'un fichier surveillé → `check.sh --fast`.
- `Stop` → `check.sh --fast` (garde-fou avant de conclure). Le rebuild complet
  n'est PAS relancé à chaque fin de tour : il reste garanti au pre-push git.

**Divergences déclarées** : `.tripwire-divergences` (committé) liste les écarts
assumés au scaffold standard — mode maison, dégradation d'environnement, alias
de dialecte. Une ligne `fichier<TAB>motif<TAB>pourquoi` ; `check.sh` rend rouge
la disparition d'un motif déclaré. Le fichier hôte d'une divergence doit être
**suivi par git** : un fichier gitignoré ne change pas l'empreinte du
skip-si-déjà-vert, donc sa perte peut passer sous un « déjà vert — skip » — il
n'est pas protégé de façon fiable. **Limite** : un écart non déclaré n'est
protégé par rien et le prochain re-scaffold l'effacera — toute divergence
délibérée se déclare au moment où on l'introduit.

### Quand invoquer les agents du projet

`.claude/agents/` contient cinq agents spécialisés au coffre :

| Agent | Quand |
|---|---|
| `niphar-test-author` | écrire ou restructurer des tests ; monter le harnais hôte quand la première logique pure arrive |
| `niphar-code-reviewer` | avant un merge vers `main` ou une release, et après tout code non trivial |
| `niphar-debugger` | build cassé, test rouge, panic, disque absent côté hôte, carte SD muette |
| `niphar-maintainer` | bump de dépendance, montée d'ESP-IDF, changement de partitions ou de sdkconfig |
| `niphar-security-auditor` | ajout d'un handler d'input externe (MSC, descripteurs, parsing SD, futur CCID/FIDO), et avant release |

### Norme TDD — nouvelle logique pure
Toute nouvelle fonction de logique pure (calcul d'adressage LBA, découpe de
transferts, parsing d'en-têtes, machines à états) : test écrit **d'abord**,
ajouté à la suite de tests de la phase rapide. Le test doit être rouge avant
l'implémentation, vert après, et parallel-safe (pas d'état global muté).

Le harnais hôte existe : `test/`, compilé par CMake avec le compilateur de la
machine, lancé par `scripts/fast.sh` **avant** le build firmware. Le ratchet est
actif (`.tripwire-testcount`, committé) et le pre-push refuse une baisse.

Un test ne vaut que s'il mord : après l'avoir écrit, introduire un bug
transitoire qui devrait le faire échouer, vérifier le rouge, revenir. C'est ce
qui distingue un test d'une assertion décorative.

Seule la logique pure entre dans `test/` — pas d'appel ESP-IDF, sinon ça ne
compile pas sur l'hôte. Cette contrainte est un outil de conception : ce qui
n'est pas testable est presque toujours ce qui mélange calcul et matériel.

### Économie de modèles (subagents)
Le pipeline check.sh permet de descendre en gamme SANS risque d'hallucination,
mais seulement là où un oracle rattrape l'erreur :
- **Modèle économique (haiku) OK** : transcription de code déjà spécifié,
  refactors mécaniques, extraction citée (`fichier:ligne` obligatoire) — le
  check, la compilation ou le recoupement des citations attrapent la dérive.
- **Jamais en dessous de sonnet** : review, audit, debug, **et l'écriture
  d'assertions de test** — une assertion tautologique ou un verdict halluciné
  passent l'oracle mécanique au vert. Le jugement ne descend pas en gamme.
- Toute tâche économique DOIT finir par `./scripts/check.sh --fast` vert, et
  un test rewiré/écrit DOIT prouver qu'il mord (bug transitoire → rouge → revert).
