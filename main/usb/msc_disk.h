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

#include <stdint.h>

#include "esp_err.h"

/* Alloue le tampon de rebond. À appeler avant l'installation du pilote USB. */
esp_err_t msc_disk_init(void);

/*
 * Compteurs de diagnostic. Ils répondent à une seule question, mais elle est
 * décisive pour le débit : les transferts passent-ils par le chemin rapide
 * (multi-secteurs, DMA direct) ou retombent-ils sur le rebond secteur par
 * secteur ? Un rapport écrasé vers le rebond explique à lui seul un débit
 * divisé par huit.
 */
typedef struct {
    uint32_t fast_sectors;    /* secteurs transférés en DMA direct */
    uint32_t bounce_sectors;  /* secteurs passés par le tampon de rebond */
    uint32_t max_bufsize;     /* plus grande taille demandée par TinyUSB */
    uint32_t not_dma_hits;    /* transferts alignés mais tampon non DMA-capable */
} msc_disk_stats_t;

void msc_disk_get_stats(msc_disk_stats_t *out);
