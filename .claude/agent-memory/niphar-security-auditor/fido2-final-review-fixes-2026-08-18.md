---
name: fido2-final-review-fixes-2026-08-18
description: Re-revue ciblée du diff 5f32bd2..9c26bb8 (9 commits, vague de correctifs de la revue finale FIDO2) — C1/I1/I4/I5/M1 vérifiés fichier par ligne ; gap de concurrence résiduel trouvé dans sec_confirm_reset().
metadata:
  type: project
---

Re-revue ciblée (pas un audit complet) du diff
`.superpowers/sdd/2026-08-17-fido2-plan-1/review-5f32bd2..9c26bb8.diff` sur
`fido2-u2f`, demandée après la vague de correctifs de la revue finale de
branche. Résultat par point (voir aussi [[ctaphid-audit-2026-08-17]] et
[[fido-key-master-audit-2026-08-18]] pour l'audit complet des tâches 2/6) :

- **C1 (nonce zéroïsé avant lecture, `main/security/u2f.c:415-441`
  `handle_register()`)** : ADRESSÉ et vérifié correct. Le nonce est copié
  dans un local AVANT `u2f_reset()`, les trois usages (`fido_key_derive`,
  `fido_key_tag`, `memcpy` vers `cred_id`) lisent bien la copie, et le local
  est effacé (`memset`) sur les DEUX chemins de sortie (échec `pk_ok`, et
  succès juste après recopie dans `cred_id`) — pas de fuite résiduelle.
  `handle_authenticate()` (`u2f.c:322` env.) n'avait PAS le même bug : il
  dérive depuis `cred_id` qui pointe dans `a->data` (le buffer APDU de
  l'appelant), jamais depuis `s_wait_cred_id` — `u2f_reset()` ne l'affecte
  pas.

- **I1 (lecture unique de `s_h`, `main/usb/hid_dispatch.c`)** : ADRESSÉ dans
  les QUATRE callbacks (`tud_hid_descriptor_report_cb`,
  `tud_hid_get_report_cb`, `tud_hid_set_report_cb`,
  `tud_hid_report_complete_cb`) — chacun charge `s_h` une seule fois dans un
  local `const hid_handlers_t *h` non volatile. Complet, aucun oubli.

- **I4 (`sec_confirm_reset()` à la bascule de mode,
  `main/usb/usb_mode.c:160`)** : placé APRÈS le court-circuit « même mode »
  (donc ne coupe pas une confirmation légitime pour le mode courant si
  l'appel ne change rien), mais AVANT `usb_device_uninstall()` (`:198`), qui
  peut échouer et restaurer l'ancien `s_mode` (`:225`,
  `usb_mode_state_on_failure`). Sur un switch qui échoue à ce stade, une
  confirmation légitimement en cours pour le mode qui reste effectivement
  actif est donc quand même effacée — régression d'ergonomie réelle, MAIS
  pas nouvelle en substance : le chemin PGP avait déjà ce comportement avant
  ce diff (`mode_pgp_stop()` → `ccid_shutdown()` pose `s_shutdown=true`
  inconditionnellement avant l'`uninstall`, ce qui déclenche le
  `sec_confirm_reset()` propre à `dongle_confirm()` dans `ccid.c:448`). Ce
  diff généralise ce même risque à OTP/FIDO, il ne l'invente pas pour PGP.

- **I5 (modèle de concurrence de `sec_confirm.c`)** : l'affirmation
  « exclusivité au RUN-TIME, garantie par `usb_mode.c` » est VRAIE et
  vérifiée par lecture directe : `hid_dispatch_set(&s_fido_handlers)`
  (`mode_fido_start()`) n'est appelé qu'APRÈS le succès de
  `usb_device_install()`, et `hid_dispatch_set(NULL)`
  (`mode_fido_stop()`/`mode_otp_stop()`) AVANT `usb_device_uninstall()` ;
  `u2f_handle_apdu()` n'est atteignable que via `hid_dispatch.c` quand la
  table FIDO est active, et le drapeau `s_busy` de `usb_mode.c` interdit deux
  bascules concurrentes. Le raisonnement tient vraiment, pas par accident,
  POUR CE QU'IL COUVRE (arm()/poll() sur ccid_worker/usb_task).

  **MAIS** — gap non couvert par la correction I5, trouvé en tâche 6 de
  cette re-revue : le paragraphe séparé de `sec_confirm.c:171-176`, qui
  prouve la sûreté SPÉCIFIQUE de `sec_confirm_reset()` contre `arm()`,
  repose sur sa propre prémisse : « every caller of reset() (dongle_confirm()
  in ccid.c) runs on the same task as arm() ». Cette prémisse est devenue
  FAUSSE dès le commit I4 (AVANT le commit I5, qui n'a pas touché ce
  paragraphe) : `usb_mode.c:160` est un NOUVEAU caller de
  `sec_confirm_reset()`, tournant sur hmi_task ou la tâche REPL — jamais sur
  ccid_worker/usb_task. Pour OTP et FIDO, ce n'est même pas une fenêtre de
  course étroite : `arm()`/`poll()` de ces deux personnalités tournent
  TOUJOURS sur usb_task, jamais sur la tâche qui appelle `usb_mode_set()`,
  donc la prémisse est structurellement fausse à chaque bascule qui quitte
  OTP ou FIDO avec une opération armée. Impact analysé : les 4 champs
  (`s_state`, `s_slot`, `s_op`, `s_armed_ms`) ne sont protégés par aucun
  verrou ; un entrelacement `reset()`/`arm()` peut produire un mélange
  PENDING/IDLE incohérent, mais ne peut PAS produire AUTHORIZED (seul
  `authorize()` l'écrit, sur une bascule PENDING->AUTHORIZED déclenchée par
  un geste réel) — direction fail-safe, pas d'escalade trouvée. Classé
  Important, pas Critique. Fix suggéré : soit documenter explicitement ce
  nouveau call site dans le paragraphe reset()-specific (comme I5 l'a fait
  pour le paragraphe arm/poll), soit appliquer le portMUX que le fichier
  prescrit lui-même « if a second poll context is ever added ».

- **Trois corrections d'une ligne + M1** : toutes vérifiées présentes et
  correctes — garde `p == NULL` dans `cbor_bytes()`
  (`main/security/cbor_enc.c:91-92`), retours `mbedtls_md_hmac_*` vérifiés
  (`main/security/fido_master.c:150-165`, `memset(out,0,32)` sur échec),
  conséquence « régénérer efface tous les credentials » documentée au site
  du désaccord de schéma (`fido_master.c:97-102`). M1 : renommage
  `USB_TASK_STACK_WORDS` → `USB_TASK_STACK_BYTES` complet, aucune référence
  résiduelle à l'ancien nom (`grep` vérifié sur `main/` et `test/`).

- **Casse nouvelle** : aucune fuite mémoire trouvée sur le nonce (zéroïsé
  sur les deux chemins). Le seul « casse » réel est le gap de concurrence
  I5/`sec_confirm_reset()` ci-dessus — fail-safe, pas un bloquant de
  release, mais à corriger avant que quelqu'un d'autre ajoute un contexte de
  poll supplémentaire en s'appuyant sur une prémisse qu'il croira encore
  vraie.

Verdict rendu : OK AVEC FIXES (mergeable ; le gap I5/reset() et la
régression d'ergonomie I4 méritent un suivi rapide, aucun des deux n'est
bloquant).
