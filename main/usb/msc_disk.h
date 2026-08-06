#pragma once

/*
 * Disque MSC — expose la microSD en blocs bruts à l'hôte USB.
 *
 * Ce module implémente les callbacks `tud_msc_*` de TinyUSB. Il ne connaît de
 * la carte que l'interface de storage/sd_card.h : ni SDMMC, ni FATFS.
 *
 * Tout ce qui arrive par ces callbacks vient de l'hôte et n'est pas digne de
 * confiance. Un LBA hors capacité ou une longueur démesurée se refuse ; il ne
 * se tronque pas en silence.
 */

#include "esp_err.h"

/* Alloue le tampon de rebond. À appeler avant l'installation du pilote USB. */
esp_err_t msc_disk_init(void);
