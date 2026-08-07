---
name: threat-model-usb-serial-jtag-download
description: L'hôte USB peut faire entrer le P4 en Joint Download Boot via RTS/DTR du CDC-ACM — le modèle « malware hôte, pas d'accès physique » ne protège donc ni la flash ni la NVS
metadata:
  type: project
---

Sur ESP32-P4, l'USB-Serial-JTAG expose une CDC-ACM dont les lignes RTS/DTR
commandent le boot : `RTS=0/DTR=1` pose le drapeau download, `RTS=1/DTR=0`
reset la puce. Un reset avec le drapeau posé démarre en Joint Download Boot.
**Aucun code firmware ne peut l'empêcher** — c'est du matériel, en amont de
l'application.

Citations vérifiées (2026-08-07) :
- ESP32-P4 TRM, chap. 53, table 53.3-2, p. 2715 — RTS/DTR → drapeau download + reset.
- ESP32-P4 TRM, chap. 11 « Chip Boot Control », p. 798 —
  `EFUSE_DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE` désactive ce forçage ;
  `EFUSE_ENABLE_SECURITY_DOWNLOAD` restreint le mode à la flash en clair ;
  `EFUSE_DIS_DOWNLOAD_MODE` le supprime définitivement.

**Why:** le modèle de menace hérité de KeSp_firmware (`docs/OPENPGP_CARD.md`)
accepte des clés en clair en NVS *parce que* leur extraction demanderait un
accès physique. Sur le coffre, l'hôte USB atteint le port 3 du hub CH334R, donc
l'USB-Serial-JTAG, donc le mode download : du malware hôte seul suffit à dumper
ou réécrire la flash. `docs/HARDWARE.md` dit « pas de mode download **matériel** »
— c'est exact et c'est précisément ce qui masque la voie logicielle.

**How to apply:** ne jamais classer une faille comme « critique parce qu'elle
permettrait d'écrire la flash » sans rappeler que l'hôte y arrive déjà sans
faille. Et au moment où les clés PGP/FIDO arriveront, exiger une décision
explicite : eFuse + Secure Boot/Flash Encryption (au prix de la seule voie de
reflash), ou ouverture des jumpers JP1/JP2 en production, ou acceptation écrite
du risque. Voir [[audit-baseline-2026-08-07]].
