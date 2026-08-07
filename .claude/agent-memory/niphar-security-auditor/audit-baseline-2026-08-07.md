---
name: audit-baseline-2026-08-07
description: Premier audit complet MSC/descripteurs/SD/console/link — ce qui a été prouvé sain, et la seule faille mémoire trouvée (course sd_probe ↔ callbacks MSC)
metadata:
  type: project
---

Premier audit de sécurité du coffre (2026-08-07, commit 2bfa6fe).

**Prouvé sain — ne pas ré-auditer de zéro tant que le fichier ne bouge pas :**
- `main/usb/msc_lba.c` — la validation LBA/offset/bufsize est correcte : tout est
  calculé en 64 bits, `lba >= sector_count` rejeté d'abord, puis `offset >= avail`,
  puis `bufsize > capacity - addr`. Aucun repliement possible. Les morceaux rendus
  ne débordent jamais le secteur ni le tampon de rebond.
- `tud_descriptor_string_cb` (`usb/usb_device.c`) — `desc[32]`, troncature à 31
  caractères, en-tête cohérent. Pas de débordement. Les champs INQUIRY à longueur
  fixe sont bien remplis à l'espace sans NUL, conformément à SPC-3.
- `main/console/console.c` — aucune commande n'écrit en flash, en NVS, ni n'expose
  de secret. `sd bench` est en lecture seule.
- `sd_card.c:check_range` — somme 64 bits, rejet et non troncature.

**La faille :** `sd probe` sur la console (accessible à l'hôte) appelle
`sd_probe()` → `release()` qui fait `free(s_card)` + `sdmmc_host_deinit()` sans
aucune exclusion mutuelle avec la tâche USB qui peut être en plein
`sdmmc_read_sectors()`. Le mutex d'ESP-IDF (`s_request_mutex`,
`components/esp_driver_sdmmc/src/sdmmc_transaction.c`) est détruit et mis à NULL
par le deinit, et `sdmmc_host_do_transaction()` le prend sans test de nullité.
Use-after-free + déréférencement NULL déclenchables depuis l'hôte.

**Why:** c'est la seule violation mémoire du dépôt à cette date, et elle vient
d'un manque de synchronisation entre deux tâches, pas d'un défaut de bornage —
le bornage, lui, est soigné partout. Chercher les prochaines failles du même
genre (état global partagé entre la tâche USB et la console) plutôt que des
dépassements de tampon.

**How to apply:** au prochain audit, vérifier d'abord si un mutex protège
désormais `s_card` ; puis se concentrer sur les nouveaux points d'entrée
(CCID/APDU, HID/CTAP, parsing d'index multi-ISO sur la SD) qui n'existaient pas
ici. Voir [[threat-model-usb-serial-jtag-download]].
