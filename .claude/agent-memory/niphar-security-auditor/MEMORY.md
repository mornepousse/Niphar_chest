# Mémoire — auditeur sécurité Niphar_chest

- [Mode download atteignable par l'hôte](threat-model-usb-serial-jtag-download.md) — RTS/DTR du CDC-ACM font entrer le P4 en download boot : la flash et la NVS ne sont pas protégées du malware hôte.
- [Baseline d'audit 2026-08-07](audit-baseline-2026-08-07.md) — ce qui est prouvé sain (msc_lba, descripteurs, console) et la course sd_probe ↔ MSC.
- [Gap concurrence sec_op_t / écran OLED](ecran-oled-sec-op-concurrency-gap.md) — armed_op()+peek() lus séparément par la tâche d'affichage, aucune garantie de paire atomique ; à revérifier à l'audit de la tâche 2 (hmi_task/screen.c).
