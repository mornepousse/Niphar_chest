#pragma once

/*
 * Accès à la microSD — SDMMC slot 0, 4 bits, accès secteur brut.
 *
 * Ce module ne monte AUCUN système de fichiers. Tant que le MSC expose les
 * blocs bruts à l'hôte, le firmware ne doit pas écrire le média en parallèle :
 * deux systèmes de fichiers sur le même support se corrompent l'un l'autre.
 * Le MSC possède la carte ; ce module se contente de servir des secteurs.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sd_protocol_types.h"

/* Chemin d'alimentation des IO SD retenu au dernier sondage. */
typedef enum {
    SD_PWR_UNKNOWN = 0,  /* aucun sondage réussi */
    SD_PWR_EXTERNAL,     /* VDDPST_5 (SD_VREF) alimenté par la carte, en 3,3 V */
    SD_PWR_ON_CHIP_LDO,  /* LDO interne canal 4 (LDO_VO4) */
} sd_pwr_path_t;

/*
 * Crée le verrou d'accès. À appeler une fois, avant tout le reste, et avant que
 * l'USB ne démarre : deux tâches se partagent la carte (les callbacks MSC et la
 * console), et un sondage concurrent d'un transfert libérerait la carte sous
 * les pieds de l'autre.
 */
esp_err_t sd_card_init(void);

/*
 * (Re)détecte la carte. Appelé au démarrage et re-déclenchable depuis la
 * console — le coffre n'a pas de card-detect matériel, la présence se constate
 * en interrogeant la carte.
 *
 * Idempotent : un sondage relâche l'état du précédent avant de recommencer.
 * Renvoie ESP_ERR_INVALID_STATE si sd_card_init() n'a pas été appelé.
 */
esp_err_t sd_probe(void);

/* Vraie seulement si le dernier sondage a abouti. */
bool sd_present(void);

/* 0 si aucune carte. */
uint32_t sd_sector_count(void);
uint32_t sd_sector_size(void);

/*
 * Lecture/écriture de secteurs. `dst`/`src` doivent être accessibles au DMA
 * (MALLOC_CAP_DMA) : c'est à l'appelant de le garantir, les buffers de TinyUSB
 * ne le sont pas nécessairement.
 *
 * Renvoie ESP_ERR_INVALID_STATE sans carte, ESP_ERR_INVALID_ARG si l'accès
 * déborde la capacité.
 */
esp_err_t sd_read_sectors(void *dst, uint32_t start_sector, uint32_t count);
esp_err_t sd_write_sectors(const void *src, uint32_t start_sector, uint32_t count);

/* Carte brute, pour l'affichage. NULL si absente. Ne pas s'en servir pour les IO. */
const sdmmc_card_t *sd_raw_card(void);

/* Par où les IO SD ont été alimentées au dernier sondage réussi. */
sd_pwr_path_t sd_pwr_path(void);
