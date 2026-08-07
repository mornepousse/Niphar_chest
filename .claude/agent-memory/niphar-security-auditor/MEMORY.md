# Mémoire — auditeur sécurité Niphar_chest

- [Mode download atteignable par l'hôte](threat-model-usb-serial-jtag-download.md) — RTS/DTR du CDC-ACM font entrer le P4 en download boot : la flash et la NVS ne sont pas protégées du malware hôte.
- [Baseline d'audit 2026-08-07](audit-baseline-2026-08-07.md) — ce qui est prouvé sain (msc_lba, descripteurs, console) et la course sd_probe ↔ MSC.
