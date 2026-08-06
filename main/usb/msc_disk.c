#include "usb/msc_disk.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "tusb.h"

#include "storage/sd_card.h"

static const char *TAG = "msc";

/* Sense keys SCSI utilisés ici (SPC-3). */
#define SCSI_SENSE_NOT_READY      0x02
#define SCSI_SENSE_MEDIUM_ERROR   0x03
#define SCSI_SENSE_ILLEGAL_REQUEST 0x05

#define ASC_MEDIUM_NOT_PRESENT    0x3A
#define ASC_LBA_OUT_OF_RANGE      0x21
#define ASC_UNRECOVERED_READ_ERROR 0x11
#define ASC_WRITE_FAULT           0x03

/*
 * Tampon de rebond, d'un secteur.
 *
 * sdmmc_read_sectors()/write_sectors() exigent de la mémoire accessible au
 * DMA ; les tampons que TinyUSB présente aux callbacks ne le sont pas
 * garantis. Et READ(10)/WRITE(10) peuvent arriver à un offset qui ne tombe pas
 * sur une frontière de secteur, ou pour une longueur qui n'en est pas un
 * multiple. Le rebond règle les deux à la fois.
 *
 * Alloué une fois, à l'init : pas de malloc dans un callback appelé à chaque
 * transfert.
 */
static uint8_t *s_bounce;
static uint32_t s_bounce_size;

static msc_disk_stats_t s_stats;

void msc_disk_get_stats(msc_disk_stats_t *out)
{
    if (out != NULL) {
        *out = s_stats;
    }
}

esp_err_t msc_disk_init(void)
{
    /*
     * La taille de secteur n'est connue qu'une fois la carte détectée. Sans
     * carte au démarrage, on prend 512 o — la valeur de toutes les cartes SD
     * courantes — et sd_probe() ne changera rien à cette hypothèse : un média
     * à secteurs plus grands serait refusé à l'usage plutôt que de déborder.
     */
    s_bounce_size = 512;
    if (sd_present() && sd_sector_size() > s_bounce_size) {
        s_bounce_size = sd_sector_size();
    }

    s_bounce = heap_caps_malloc(s_bounce_size, MALLOC_CAP_DMA);
    if (s_bounce == NULL) {
        ESP_LOGE(TAG, "tampon de rebond (%" PRIu32 " o) : plus de mémoire DMA", s_bounce_size);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Callbacks SCSI                                                            */
/* ------------------------------------------------------------------------ */

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16],
                        uint8_t product_rev[4])
{
    (void)lun;
    /* Champs à longueur fixe, complétés par des espaces et NON terminés par NUL. */
    memcpy(vendor_id,  "Niphar  ", 8);
    memcpy(product_id, "Coffre microSD  ", 16);
    memcpy(product_rev, "0001", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    if (!sd_present()) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0x00);
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    /*
     * Sans carte, on annonce une capacité nulle plutôt que de mentir : l'hôte
     * a déjà appris l'absence de média par TEST UNIT READY.
     */
    *block_count = sd_present() ? sd_sector_count() : 0;
    *block_size = sd_present() ? (uint16_t)sd_sector_size() : 512;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun;
    (void)power_condition;
    (void)start;

    /*
     * L'éjection logicielle est acceptée pour que l'hôte puisse démonter
     * proprement, mais elle ne coupe rien : le contrat matériel impose de
     * retirer la carte hors tension (docs/HARDWARE.md). Prétendre éjecter
     * serait plus trompeur qu'utile.
     */
    if (load_eject) {
        ESP_LOGI(TAG, "éjection demandée par l'hôte — la carte se retire hors tension");
    }
    return true;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    return sd_present();
}

/*
 * Cœur commun aux deux sens.
 *
 * `lba` et `offset` viennent de l'hôte : l'adresse absolue est calculée en 64
 * bits, sinon lba * secteur déborde dès 4 Gio et un test de bornes naïf
 * passerait.
 *
 * Renvoie le nombre d'octets traités, ou -1 (l'appelant pose le sense).
 */
static int32_t transfer(bool writing, uint32_t lba, uint32_t offset,
                        uint8_t *buffer, uint32_t bufsize)
{
    const uint32_t ss = sd_sector_size();
    if (ss == 0 || s_bounce == NULL || ss > s_bounce_size) {
        return -1;
    }

    if (bufsize > s_stats.max_bufsize) {
        s_stats.max_bufsize = bufsize;
    }

    uint64_t addr = (uint64_t)lba * (uint64_t)ss + (uint64_t)offset;
    const uint64_t capacity = (uint64_t)sd_sector_count() * (uint64_t)ss;
    if (addr + (uint64_t)bufsize > capacity) {
        return -1;   /* hors média : refusé, pas tronqué */
    }

    uint32_t done = 0;
    while (done < bufsize) {
        const uint32_t sector = (uint32_t)(addr / ss);
        const uint32_t in_sector = (uint32_t)(addr % ss);
        uint32_t chunk = ss - in_sector;
        if (chunk > bufsize - done) {
            chunk = bufsize - done;
        }

        /*
         * Chemin rapide : transfert aligné, secteurs entiers, tampon déjà
         * accessible au DMA. C'est le cas courant, et il évite une recopie par
         * secteur.
         */
        if (in_sector == 0 && chunk == ss) {
            uint32_t whole = (bufsize - done) / ss;
            if (whole >= 1 && esp_ptr_dma_capable(buffer + done)) {
                esp_err_t err = writing
                    ? sd_write_sectors(buffer + done, sector, whole)
                    : sd_read_sectors(buffer + done, sector, whole);
                if (err != ESP_OK) {
                    return -1;
                }
                const uint32_t moved = whole * ss;
                s_stats.fast_sectors += whole;
                done += moved;
                addr += moved;
                continue;
            }
            if (whole >= 1) {
                s_stats.not_dma_hits++;
            }
        }

        /* Chemin lent : un secteur via le rebond. */
        s_stats.bounce_sectors++;
        if (writing) {
            if (chunk != ss) {
                /* Écriture partielle : lire-modifier-écrire, sinon on efface le reste. */
                if (sd_read_sectors(s_bounce, sector, 1) != ESP_OK) {
                    return -1;
                }
            }
            memcpy(s_bounce + in_sector, buffer + done, chunk);
            if (sd_write_sectors(s_bounce, sector, 1) != ESP_OK) {
                return -1;
            }
        } else {
            if (sd_read_sectors(s_bounce, sector, 1) != ESP_OK) {
                return -1;
            }
            memcpy(buffer + done, s_bounce + in_sector, chunk);
        }

        done += chunk;
        addr += chunk;
    }

    return (int32_t)bufsize;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer,
                          uint32_t bufsize)
{
    if (!sd_present()) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0x00);
        return -1;
    }
    int32_t n = transfer(false, lba, offset, (uint8_t *)buffer, bufsize);
    if (n < 0) {
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, ASC_UNRECOVERED_READ_ERROR, 0x00);
    }
    return n;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer,
                           uint32_t bufsize)
{
    if (!sd_present()) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0x00);
        return -1;
    }
    int32_t n = transfer(true, lba, offset, buffer, bufsize);
    if (n < 0) {
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, ASC_WRITE_FAULT, 0x00);
    }
    return n;
}

/*
 * Commandes SCSI que TinyUSB ne traite pas lui-même. On n'en implémente
 * aucune : renvoyer -1 avec ILLEGAL REQUEST est la réponse honnête, et évite
 * d'inventer un comportement qu'un hôte prendrait au sérieux.
 */
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;
    ESP_LOGD(TAG, "commande SCSI non gérée : 0x%02x", scsi_cmd[0]);
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
    return -1;
}
