# Mémoire — auditeur sécurité Niphar_chest

- [Mode download atteignable par l'hôte](threat-model-usb-serial-jtag-download.md) — RTS/DTR du CDC-ACM font entrer le P4 en download boot : la flash et la NVS ne sont pas protégées du malware hôte.
- [Baseline d'audit 2026-08-07](audit-baseline-2026-08-07.md) — ce qui est prouvé sain (msc_lba, descripteurs, console) et la course sd_probe ↔ MSC.
- [Gap concurrence sec_op_t / écran OLED](ecran-oled-sec-op-concurrency-gap.md) — armed_op()+peek() lus séparément par la tâche d'affichage, aucune garantie de paire atomique ; à revérifier à l'audit de la tâche 2 (hmi_task/screen.c).
- [Audit ctaphid.c — tâche 2 FIDO2 (2026-08-17)](ctaphid-audit-2026-08-17.md) — bornage OK, mutant équivalent confirmé par ASan, gap `out` non-zéroé si a/pkt NULL, brief contenait son propre test aveugle (CID gros-boutiste mal placé).
- [Audit fido_key.c/fido_master.c — tâche 6 FIDO2 (2026-08-18)](fido-key-master-audit-2026-08-18.md) — séparation de domaine et fake_hmac vérifiés par fuzzing indépendant (OK), esp_fill_random = TRNG confirmé sur P4 par défaut (lemia), gap de concurrence non protégé sur le lazy-init NVS de la clé maîtresse (à revérifier tâche 7).
- [Re-revue correctifs finale FIDO2 (2026-08-18)](fido2-final-review-fixes-2026-08-18.md) — C1/I1/M1/3 one-liners vérifiés corrects ; I4 a une régression d'ergonomie mineure (reset avant échec possible) ; I5 laisse un gap : `sec_confirm_reset()` a un nouveau caller cross-task (usb_mode.c) que la preuve reset()-vs-arm() de sec_confirm.c:171-176 ne couvre pas — fail-safe, à corriger.
