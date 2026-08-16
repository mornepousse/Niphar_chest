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
#include "nvs_flash.h"

#include "board.h"
#include "console/console.h"
#include "sec_gate.h"
#include "storage/sd_card.h"
#include "usb/usb_mode.h"

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
#if BOARD_HAS_SD
    esp_err_t sd_err = sd_card_init();
    if (sd_err != ESP_OK) {
        ESP_LOGE(TAG, "verrou carte SD : %s", esp_err_to_name(sd_err));
    }
    (void)sd_probe();
#else
    ESP_LOGI(TAG, "pas de microSD sur cette carte — sondage sauté");
#endif

    /*
     * NVS avant l'USB : c'est le socle flash dont les DO/PIN/clés OpenPGP
     * (mode_pgp_data_load(), tâche 12) et les secrets sec_store (mode OTP)
     * ont besoin pour persister — mais rien ne les charge encore ici, ce
     * n'est que la partition NVS qui s'ouvre, pas un mode qui s'active.
     * Découvert en câblant la tâche 12 : sans cet appel, nvs_open() échoue
     * ESP_ERR_NVS_NOT_INITIALIZED, PUT DATA et VERIFY (pin persist)
     * réussissent en RAM le temps de la session mais rien ne survit à un
     * reset. Pas de bouton reset sur le coffre (docs/HARDWARE.md), donc
     * jamais rencontré avant ce test bout en bout.
     */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS : partition à réinitialiser (%s)", esp_err_to_name(nvs_err));
        nvs_err = nvs_flash_erase();
        if (nvs_err == ESP_OK) {
            nvs_err = nvs_flash_init();
        }
    }
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS indisponible : %s — PGP/OTP resteront en RAM seule, sans persistance",
                 esp_err_to_name(nvs_err));
    }

    /*
     * L'USB ensuite, mais sans exposer de fonction : le coffre démarre en
     * USB_MODE_NONE (voir usb/usb_mode.h) et c'est la console — ou, plus tard,
     * le clavier via le lien S3 — qui demandera explicitement un mode. Un
     * échec ici ne doit pas empêcher la console de démarrer.
     */
    esp_err_t usb_err = usb_mode_init();
    if (usb_err != ESP_OK) {
        ESP_LOGE(TAG, "USB indisponible : %s — la console reste le recours",
                 esp_err_to_name(usb_err));
    }

    /*
     * La source de confirmation avant la console : « sec confirm » (kit) en
     * dépend directement, et le journal de démarrage doit dire d'où viendra
     * l'appui avant que quiconque puisse en armer une.
     */
    esp_err_t gate_err = sec_gate_init();
    if (gate_err != ESP_OK) {
        ESP_LOGE(TAG, "sec_gate indisponible : %s", esp_err_to_name(gate_err));
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
