#include "storage/sd_card.h"

#include <stdlib.h>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

#include "board.h"

static const char *TAG = "sd";

/*
 * Alimentation des IO SD sur ESP32-P4 — pourquoi ce module sonde deux fois.
 *
 * « ESP32-P4 SDMMC Host requires the IO voltage to be supplied externally via
 * the VDDPST_5 (SD_VREF) pin. If the design doesn't require the higher speed SD
 * modes, this pin can be simply connected to the 3.3V supply. »
 *   — ESP-IDF Programming Guide, SDMMC Host Driver (ESP32-P4),
 *     § Configuring Voltage Level.
 *
 * Le LDO interne (canal 4, LDO_VO4) ne sert donc qu'aux modes 1,8 V. Le coffre
 * est en IO fixe 3,3 V sans SDR104 et déclare LDO_VO4 non câblé
 * (docs/HARDWARE.md) : l'alimentation externe est l'hypothèse attendue.
 *
 * Mais le contrat matériel ne dit rien de VDDPST_5 lui-même, et le BSP du kit
 * de dev, lui, configure le LDO canal 4. Plutôt que de parier, on essaie
 * l'alimentation externe d'abord, puis le LDO, et on journalise ce qui a
 * marché : le premier bring-up répond ainsi à la question au lieu de la
 * reporter.
 */
#define SD_LDO_CHANNEL 4

static sdmmc_card_t *s_card;
static sd_pwr_ctrl_handle_t s_pwr;
static sd_pwr_path_t s_path = SD_PWR_UNKNOWN;
static bool s_host_ready;

/*
 * Verrou d'accès à la carte.
 *
 * Deux tâches touchent cet état : la tâche USB, qui lit et écrit des secteurs
 * depuis les callbacks MSC, et la console, d'où l'utilisateur peut relancer un
 * sondage. Sans verrou, un « sd probe » pendant un transfert libère `s_card` et
 * détruit le mutex interne du pilote SDMMC sous les pieds de l'autre tâche —
 * usage après libération sur le tas, et prise d'un sémaphore déjà détruit.
 *
 * Le P4 est bicœur et la tâche USB n'est pas épinglée : ce n'est pas une course
 * théorique. Et la console est accessible à l'hôte, donc le déclencheur l'est
 * aussi.
 *
 * Le verrou doit couvrir l'appel SDMMC entier, pas seulement le contrôle de
 * bornes : la fenêtre s'ouvre pendant que le pilote rend la main.
 *
 * Alloué statiquement — une création qui échoue laisserait un verrou nul, donc
 * exactement le bug qu'on ferme.
 */
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;

/* À n'appeler qu'en tenant s_lock. */
static void release(void)
{
    if (s_card != NULL) {
        free(s_card);
        s_card = NULL;
    }
    if (s_host_ready) {
        /* Conditionné : au premier sondage l'hôte n'a jamais été initialisé, et
         * un deinit inconditionnel échouerait légitimement à chaque démarrage. */
        esp_err_t err = sdmmc_host_deinit();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "deinit hôte SDMMC : %s", esp_err_to_name(err));
        }
        s_host_ready = false;
    }
    if (s_pwr != NULL) {
        esp_err_t err = sd_pwr_ctrl_del_on_chip_ldo(s_pwr);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "libération du LDO : %s", esp_err_to_name(err));
        }
        s_pwr = NULL;
    }
    s_path = SD_PWR_UNKNOWN;
}

/* Une tentative complète de détection. Laisse l'état propre en cas d'échec. */
static esp_err_t probe_once(bool use_on_chip_ldo)
{
    esp_err_t err;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    if (use_on_chip_ldo) {
        sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = SD_LDO_CHANNEL };
        err = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "LDO interne canal %d indisponible : %s",
                     SD_LDO_CHANNEL, esp_err_to_name(err));
            return err;
        }
        host.pwr_ctrl_handle = s_pwr;
    }

    err = sdmmc_host_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init hôte SDMMC : %s", esp_err_to_name(err));
        goto fail;
    }
    s_host_ready = true;

    /*
     * Slot 0 passe par l'IO MUX ; les pins déclarés ici sont précisément ceux
     * de l'IO MUX. On les nomme quand même : board.h reste la seule source du
     * brochage, et un jour où le câblage bougerait, l'écart se verrait ici.
     */
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = BOARD_SD_CLK;
    slot.cmd = BOARD_SD_CMD;
    slot.d0 = BOARD_SD_D0;
    slot.d1 = BOARD_SD_D1;
    slot.d2 = BOARD_SD_D2;
    slot.d3 = BOARD_SD_D3;
    slot.cd = SDMMC_SLOT_NO_CD;   /* pas de contact de détection sur le coffre */
    slot.wp = SDMMC_SLOT_NO_WP;
    slot.width = BOARD_SD_BUS_WIDTH;
    slot.flags = 0;               /* pull-ups 10k externes, cf. docs/HARDWARE.md */

    err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_0, &slot);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init slot SDMMC : %s", esp_err_to_name(err));
        goto fail;
    }

    s_card = calloc(1, sizeof(sdmmc_card_t));
    if (s_card == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    err = sdmmc_card_init(&host, s_card);
    if (err != ESP_OK) {
        free(s_card);
        s_card = NULL;
        goto fail;
    }

    s_path = use_on_chip_ldo ? SD_PWR_ON_CHIP_LDO : SD_PWR_EXTERNAL;
    return ESP_OK;

fail:
    release();
    return err;
}

esp_err_t sd_card_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    return s_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t sd_probe(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;   /* sd_card_init() n'a pas été appelé */
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);

    release();

    esp_err_t err = probe_once(false);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "carte détectée, IO alimentées par la carte (VDDPST_5 en 3,3 V)");
    } else {
        ESP_LOGW(TAG, "sondage sans LDO : %s — nouvel essai avec le LDO interne",
                 esp_err_to_name(err));
        err = probe_once(true);
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "carte détectée via le LDO interne canal %d.",
                     SD_LDO_CHANNEL);
            ESP_LOGW(TAG, "Cette carte alimente donc VDDPST_5 par LDO_VO4 — or le");
            ESP_LOGW(TAG, "coffre declare LDO_VO4 non cable : a verifier avant fab.");
        }
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s, %llu Mo, bus %d bits, %d kHz",
                 s_card->cid.name,
                 ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) / (1024 * 1024),
                 s_card->log_bus_width ? (1 << s_card->log_bus_width) : 1,
                 s_card->max_freq_khz);
    } else {
        ESP_LOGW(TAG, "aucune carte (%s) — le coffre reste utilisable, la SD non",
                 esp_err_to_name(err));
    }

    xSemaphoreGive(s_lock);
    return err;
}

bool sd_present(void)
{
    return s_card != NULL;
}

uint32_t sd_sector_count(void)
{
    return s_card != NULL ? s_card->csd.capacity : 0;
}

uint32_t sd_sector_size(void)
{
    return s_card != NULL ? s_card->csd.sector_size : 0;
}

/* Bornage commun aux deux sens. Un accès hors capacité est refusé, pas tronqué. */
static esp_err_t check_range(uint32_t start_sector, uint32_t count)
{
    if (s_card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Somme vérifiée sur 64 bits : sur 32 elle pourrait déborder et passer le test. */
    if ((uint64_t)start_sector + (uint64_t)count > (uint64_t)s_card->csd.capacity) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/*
 * Le verrou couvre le contrôle de bornes ET le transfert : c'est pendant
 * l'appel SDMMC que la tâche console pourrait libérer la carte sous les pieds
 * de la tâche USB.
 */
esp_err_t sd_read_sectors(void *dst, uint32_t start_sector, uint32_t count)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_err_t err = check_range(start_sector, count);
    if (err == ESP_OK) {
        err = sdmmc_read_sectors(s_card, dst, start_sector, count);
    }

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t sd_write_sectors(const void *src, uint32_t start_sector, uint32_t count)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_err_t err = check_range(start_sector, count);
    if (err == ESP_OK) {
        err = sdmmc_write_sectors(s_card, src, start_sector, count);
    }

    xSemaphoreGive(s_lock);
    return err;
}

const sdmmc_card_t *sd_raw_card(void)
{
    return s_card;
}

sd_pwr_path_t sd_pwr_path(void)
{
    return s_path;
}
