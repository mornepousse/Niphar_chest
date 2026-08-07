/*
 * Coffre Niphar — point d'entrée.
 *
 * Le coffre ne s'éveille qu'en filaire : quand app_main() tourne, l'USB est
 * présent par construction. Voir docs/HARDWARE.md.
 */

#include <inttypes.h>

#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"

#include "board.h"
#include "console/console.h"
#include "storage/sd_card.h"
#include "usb/usb_device.h"

static const char *TAG = "niphar";

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        flash_size = 0;
    }

    /* La carte en clair dès la première ligne : les deux firmwares se
     * ressemblent, et savoir lequel tourne évite des heures de diagnostic. */
    ESP_LOGI(TAG, "coffre Niphar %s — carte %s", NIPHAR_VERSION, BOARD_NAME);
    /* esp_chip_info_t.revision est au format MXX : major = /100, minor = %100. */
    ESP_LOGI(TAG, "ESP32-P4 rev v%d.%d, %d coeur(s), flash %" PRIu32 " Mo",
             chip.revision / 100, chip.revision % 100,
             chip.cores, flash_size / (1024 * 1024));
    ESP_LOGI(TAG, "heap libre : %" PRIu32 " o", esp_get_free_heap_size());
    ESP_LOGI(TAG, "microSD attendue sur CLK=%d CMD=%d D0-D3=%d,%d,%d,%d (slot 0, %d-bit)",
             BOARD_SD_CLK, BOARD_SD_CMD,
             BOARD_SD_D0, BOARD_SD_D1, BOARD_SD_D2, BOARD_SD_D3,
             BOARD_SD_BUS_WIDTH);
#if BOARD_LINK_AVAILABLE
    ESP_LOGI(TAG, "lien S3 sur CS=%d MOSI=%d SCK=%d MISO=%d IRQ=%d",
             BOARD_LINK_CS, BOARD_LINK_MOSI, BOARD_LINK_SCK,
             BOARD_LINK_MISO, BOARD_LINK_IRQ);
#else
    ESP_LOGI(TAG, "lien S3 : indisponible sur cette carte");
#endif

    /*
     * Une carte absente n'est pas une erreur fatale : le coffre doit rester
     * flashable et interrogeable sans elle. On journalise et on continue.
     */
    esp_err_t sd_err = sd_card_init();
    if (sd_err != ESP_OK) {
        ESP_LOGE(TAG, "verrou carte SD : %s", esp_err_to_name(sd_err));
    }
    (void)sd_probe();

    /*
     * Le MSC ensuite. Il énumère même sans carte : l'hôte apprendra l'absence
     * de média par TEST UNIT READY, ce qui vaut mieux qu'un périphérique
     * fantôme. Un échec ici ne doit pas empêcher la console de démarrer.
     */
    esp_err_t usb_err = usb_device_start();
    if (usb_err != ESP_OK) {
        ESP_LOGE(TAG, "USB indisponible : %s — la console reste le recours",
                 esp_err_to_name(usb_err));
    }

    /*
     * La console en dernier, et elle ne rend pas la main. Si son démarrage
     * échoue, on le dit — c'est le seul moyen de diagnostic du coffre, perdre
     * silencieusement serait pire que tout.
     */
    esp_err_t err = console_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "console indisponible : %s", esp_err_to_name(err));
    }
}
