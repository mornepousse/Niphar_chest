---
name: sec-confirm-reset-race-closed-2026-08-18
description: Fermeture des deux points parqués par fido2-final-review-fixes-2026-08-18 (gap reset()/arm() et effacement de confirmation sur échec de bascule) — verrou portMUX ajouté, reset() déplacé après l'uninstall.
metadata:
  type: project
---

Suite directe de [[fido2-final-review-fixes-2026-08-18]] : les deux points
parqués par cette re-revue (gap de concurrence I5/`sec_confirm_reset()`, et
la régression d'ergonomie I4) ont été fermés le 2026-08-18, commits
`8fefa12` (sec_confirm.c) et `f6f60d0` (usb_mode.c) sur `fido2-u2f`.

**Point 1 — le gap de concurrence était plus grave que "fail-safe".** En
creusant l'entrelacement champ par champ (deux écrivains VRAIMENT concurrents
sur deux cœurs HP, pas juste un lecteur qui tombe entre les stores d'un seul
écrivain), un cas non fail-safe existe : `s_state` gagné par `arm()`
(PENDING) combiné à `s_slot` resté à la valeur de `reset()` (0). Si un appui
physique réel tombe dans cette fenêtre, `authorize()` accorde légitimement
et `poll()` rend le slot 0 — pas le slot réellement armé. Pour l'OTP
(`otp_hid.c`, index sec_store 0 et 1), ce n'est pas un cas d'école : un octroi
peut s'appliquer au mauvais slot HMAC sur un geste réel. Ça contredit la
conclusion précédente ("ne peut pas produire AUTHORIZED... fail-safe") : elle
restait vraie pour AUTHORIZED-sans-geste, mais pas pour
AUTHORIZED-sur-le-mauvais-slot avec un geste réel — nuance qui n'avait pas été
poussée jusqu'au bout lors du premier passage.

Fix choisi (option a, pas b) : `arm()`/`reset()`/`poll()`/`authorize()`
passent sous un spinlock `portMUX_TYPE`, gate `#ifndef TEST_HOST` (motif déjà
en place dans `fido_master.c`/`sec_store.c`). `peek()`/`peek_labeled()`
restent hors verrou — leur analyse de torn read (un seul écrivain actif à la
fois, désormais garanti par le verrou) reste valide sans modification.

Option (b) — déplacer l'appel de `usb_mode.c` dans chaque `mode_*_stop()` —
a été vérifiée et REJETÉE : `mode_pgp_stop()`/`mode_otp_stop()`/
`mode_fido_stop()` tournent toutes SYNCHRONES, sur la MÊME tâche que la
fonction de bascule elle-même (hmi_task ou tâche REPL) — les déplacer d'une
fonction à l'autre ne change pas le contexte d'exécution. Pour qu'un
déplacement fonctionne, il aurait fallu une tâche persistante côté OTP/FIDO à
qui déléguer le reset ; seul PGP en a une (`ccid_worker`), et elle se
nettoie déjà elle-même via `dongle_confirm()` qui teste `s_shutdown` en
boucle (`ccid.c:447-450`) — mécanisme correct et antérieur à I4, jamais le
problème.

**Point 2 — corrigé, pas seulement documenté.** `sec_confirm_reset()` central
déplacé de juste avant `mode_*_stop()`/`usb_device_uninstall()` à juste après
le bloc `if (err != ESP_OK)` qui suit l'uninstall — donc seulement quand la
désinstallation a RÉELLEMENT réussi (plus de chemin de retour arrière),
toujours avant toute installation du nouveau mode (pas de fenêtre inverse).
Un échec de désinstallation préserve désormais la confirmation légitime du
mode qui reste actif.

**Vérification matérielle** : build + flash sur wt9932_key réel (MAC
`30:ed:a0:e0:bc:5f`), cycle OTP→FIDO→PGP→NONE→OTP→NONE sur console série,
aucun panic/deadlock du nouveau spinlock, `ccid_worker` observé sortir
proprement à chaque sortie de PGP.

**Point à surveiller pour le plan 2** : `ctap2.c` deviendra un 3ᵉ/4ᵉ appelant
potentiel de `sec_confirm`. Le verrou couvre n'importe quel nouvel appelant
quelle que soit sa tâche, TANT QU'il passe par les fonctions publiques de
`sec_confirm.h` — pas d'accès direct aux champs statiques.

**Bug pré-existant repéré en marge, non corrigé (hors scope de ce ticket)** :
quand `usb_device_uninstall()` échoue, `mode_*_stop()` a déjà tourné (retrait
du dispatch CCID/HID) alors que le périphérique reste électriquement présent
sur le bus (rien n'a été détaché). L'hôte peut continuer d'envoyer des
rapports HID vers un mode dont le répartiteur est déjà vide — silencieusement
ignorés plutôt que traités. À vérifier si ça mérite un ticket séparé.
