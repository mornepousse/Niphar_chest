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

static const char *TAG = "niphar";

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        flash_size = 0;
    }

    ESP_LOGI(TAG, "coffre Niphar %s", NIPHAR_VERSION);
    /* esp_chip_info_t.revision est au format MXX : major = /100, minor = %100. */
    ESP_LOGI(TAG, "ESP32-P4 rev v%d.%d, %d coeur(s), flash %" PRIu32 " Mo",
             chip.revision / 100, chip.revision % 100,
             chip.cores, flash_size / (1024 * 1024));
    ESP_LOGI(TAG, "heap libre : %" PRIu32 " o", esp_get_free_heap_size());
    ESP_LOGI(TAG, "microSD attendue sur CLK=%d CMD=%d D0-D3=%d,%d,%d,%d (slot 0, %d-bit)",
             BOARD_SD_CLK, BOARD_SD_CMD,
             BOARD_SD_D0, BOARD_SD_D1, BOARD_SD_D2, BOARD_SD_D3,
             BOARD_SD_BUS_WIDTH);
}
