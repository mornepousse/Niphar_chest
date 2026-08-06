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

`main/board.h` porte des `_Static_assert`, et `scripts/fast.sh` des greps, qui
font échouer le build sur ces deux points. Ne pas les contourner.

**Le kit de dev pardonne, le coffre non.** Le JC-ESP32P4-M3-DEV a un CH340C et
un bouton BOOTMODE ; ces règles n'y seront donc jamais vérifiées à l'exécution.
Elles tiennent par construction, pas par expérience.

## Build

Cible unique : le kit et le coffre partagent le même pinout, il n'y a rien à
faire varier.

```bash
source ~/esp/esp-idf/export.sh
idf.py build
```

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

### Norme TDD — nouvelle logique pure
Toute nouvelle fonction de logique pure (calcul d'adressage LBA, découpe de
transferts, parsing d'en-têtes, machines à états) : test écrit **d'abord**,
ajouté à la suite de tests de la phase rapide. Le test doit être rouge avant
l'implémentation, vert après, et parallel-safe (pas d'état global muté).

Il n'y a pas encore de harnais de tests hôte : le ratchet de `check.sh` est
inerte et la phase rapide se réduit aux garde-fous plus le build. Le premier
morceau de logique pure — vraisemblablement l'adressage LBA du MSC — doit
arriver avec son harnais, sur le modèle du `test/` de KeSp_firmware.

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
