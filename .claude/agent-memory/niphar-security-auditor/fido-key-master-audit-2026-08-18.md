---
name: fido-key-master-audit-2026-08-18
description: Audit tâche 6 FIDO2 (66472f8) — fido_key.c (dérivation pure) et fido_master.c (clé maîtresse NVS) ; séparation de domaine vérifiée par fuzzing indépendant, RNG P4 confirmé TRNG par défaut, gap de concurrence non protégé sur le lazy-init NVS
metadata:
  type: project
---

Commit `66472f8` (tâche 6 du plan FIDO2, branche `fido2-u2f`) ajoute
`main/security/fido_key.{c,h}` (pur, testé sur l'hôte) et
`main/security/fido_master.{c,h}` (non pur, HMAC-SHA256 mbedtls + clé
maîtresse 32o en NVS générée par `esp_fill_random`). Aucun appelant pour
l'instant — `grep -rn "fido_key_check\|fido_master_hmac" main/` ne remonte
que les définitions elles-mêmes. La tâche 7 (signature makeCredential/
getAssertion) sera le premier appelant réel et le premier test de bout en
bout du chemin NVS.

**Vérifications faites au-delà de la lecture du diff** (à ne pas refaire à
l'identique si la tâche 7 ne touche pas fido_key.c/fido_master.c) :

1. **Séparation de domaine** : `build_message()` (fido_key.c:16-22) place le
   préfixe en `msg[0]`, donc en tout premier octet du message HMAC — c'est
   la position testée par `test_key_and_tag_are_different_derivations`.
   Fuzzing indépendant (2000 messages aléatoires × 49 positions × 8 bits =
   784 000 essais de flip d'un seul bit sur `fake_hmac`) : 0 collision, y
   compris sur les octets du milieu que le rapport de l'implémenteur
   n'avait pas testés explicitement (lui n'avait prouvé que le premier et
   le dernier octet). Confirmé aussi sur 5000 tirages aléatoires du
   scénario réel (préfixe seul différent, comparaison sur les 16 premiers
   octets comme le fait le test) : 0 collision.

2. **`fake_hmac` ne masque pas de défaut** : confirmé en recompilant des
   copies mutées de `fido_key.c` hors dépôt (scratchpad, pas de modif du
   repo) : préfixes identiques → 1 échec (le bon test) ; `rp_id_hash`
   retiré du message → exactement 3 échecs, les mêmes que ceux annoncés par
   l'implémenteur (`test_credential_of_another_rp_is_rejected`,
   `test_derive_differs_by_rp_id_hash`, `test_tampered_tag_is_rejected`) ;
   comparaison à temps constant remplacée par un `memcmp` à sortie
   précoce → reste verte (mutant équivalent fonctionnellement, confirmé).
   Les trois affirmations du rapport de tâche étaient exactes, pas
   hallucinées.

3. **RNG du master key (fido_master.c:54, `esp_fill_random`)** : confirmé
   TRNG sur ESP32-P4 **par défaut**, via lemia (ESP-IDF Programming Guide
   v6.0.2, § Random Number Generation - ESP32-P4, doc_id 2417, section
   « Startup ») : contrairement aux autres puces Espressif où le hasard
   matériel n'est garanti « vrai » que si Wi-Fi/BT est actif ou que l'appli
   rappelle explicitement `bootloader_random_enable()`, sur P4 « the High
   Speed ADC is not available. Hence, the non-RF internal entropy source
   (SAR ADC) is kept enabled by default at the time of application
   startup ». Donc `esp_fill_random()` appelé tardivement (au premier
   HMAC, pas au boot) reste bien un TRNG sur cette cible précise — pas un
   pseudo-aléa. **Ne pas généraliser cette conclusion à un S3/C6/C3** :
   sur ces puces la garantie dépend de Wi-Fi/BT actif, absent d'un
   contexte filaire comme celui du coffre.

**Gap trouvé, pas encore exploitable (rien n'appelle ce code)** :
`fido_master_ensure_loaded()` (fido_master.c:43-69) fait un lazy-init
« lire NVS → si absent, générer + sauvegarder » sur un flag global
`s_master_key_loaded` et un buffer global `s_master_key`, **sans mutex**.
Le reste du module security/ protège explicitement ce genre d'état partagé
entre tâches (`ccid.c` a `s_defer_lock`, `sec_confirm.c` documente une
section critique portMUX) — l'absence de tout commentaire ou verrou ici
détonne avec la convention du projet. Pas un bug aujourd'hui (aucun
appelant), mais **à vérifier explicitement quand la tâche 7 câble
`fido_master_hmac` dans ctap2.c/ctaphid.c** : si l'appel peut venir de plus
d'une tâche FreeRTOS (ou si CTAPHID traite plusieurs requêtes concurrentes),
deux premiers appels concurrents peuvent générer deux clés différentes et
écrire l'une par-dessus l'autre en RAM pendant que l'autre calcule déjà un
HMAC avec l'ancienne valeur — corruption d'état silencieuse. Voir aussi
[[ctaphid-audit-2026-08-17]] pour le modèle de tâche CTAPHID.

Autres constats mineurs (non bloquants) : `mbedtls_md_setup()`
(fido_master.c:78) et `mbedtls_md_info_from_type()` (ligne 77) ne sont pas
vérifiés en retour, contrairement à `cr_hmac.c` qui vérifie `info != NULL`
et le code de retour de `mbedtls_md_hmac()` — risque quasi nul ici (SHA256
toujours compilé dans ce projet) mais incohérent avec l'idiome établi.
Le champ `"fido_master_ver"` passé à `nvs_load_blob_with_total`/
`nvs_save_blob_with_total` n'est jamais réellement validé au chargement
(jeté dans `unused_total`) — nommage qui suggère un contrôle de schéma qui
n'existe pas ; sans conséquence tant que `FIDO_MASTER_KEY_LEN` reste fixe
(le contrôle de taille générique de `nvs_utils.c` couvre déjà la
corruption/désynchronisation de taille).

128 bits de tag (fido_key.c:44) : forge = attaque en ligne uniquement
(pas d'accès offline à K_maître dans ce modèle de menace — malware hôte,
pas accès physique), donc 2^128 tentatives via l'oracle USB est hors de
portée par plusieurs ordres de grandeur au-delà de ce qui est jamais
considéré praticable en cryptographie. Choix défendable, pas un défaut.
