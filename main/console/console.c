#include "console/console.h"

#include <stdio.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdmmc_cmd.h"

#include "storage/sd_card.h"

static const char *TAG = "console";

static const char *pwr_path_str(sd_pwr_path_t p)
{
    switch (p) {
    case SD_PWR_EXTERNAL:    return "externe (VDDPST_5 en 3,3 V)";
    case SD_PWR_ON_CHIP_LDO: return "LDO interne canal 4 (LDO_VO4)";
    default:                 return "inconnu";
    }
}

static int cmd_sd(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage : sd info | sd probe\n");
        return 1;
    }

    if (strcmp(argv[1], "probe") == 0) {
        esp_err_t err = sd_probe();
        printf("sondage : %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }

    if (strcmp(argv[1], "info") == 0) {
        const sdmmc_card_t *card = sd_raw_card();
        if (card == NULL) {
            printf("aucune carte détectée.\n");
            printf("Le coffre n'a pas de card-detect : insère la carte HORS TENSION,\n");
            printf("puis relance un cycle d'alimentation (« sd probe » à chaud peut\n");
            printf("échouer, et l'insertion sous tension n'est pas dans le contrat).\n");
            return 1;
        }
        sdmmc_card_print_info(stdout, card);
        printf("secteurs      : %" PRIu32 " de %" PRIu32 " o\n",
               sd_sector_count(), sd_sector_size());
        printf("alim. des IO  : %s\n", pwr_path_str(sd_pwr_path()));
        printf("propriétaire  : firmware (aucun FAT monté ; le MSC prendra la main)\n");
        return 0;
    }

    printf("sous-commande inconnue : %s\n", argv[1]);
    return 1;
}

esp_err_t console_start(void)
{
    esp_console_repl_t *repl = NULL;

    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "niphar>";
    repl_config.max_cmdline_length = 128;

    esp_console_dev_usb_serial_jtag_config_t hw_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "création du REPL : %s", esp_err_to_name(err));
        return err;
    }

    err = esp_console_register_help_command();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commande help : %s", esp_err_to_name(err));
        return err;
    }

    const esp_console_cmd_t sd_cmd = {
        .command = "sd",
        .help = "Carte microSD : « sd info » (état) ou « sd probe » (re-détection)",
        .hint = "info|probe",
        .func = &cmd_sd,
    };
    err = esp_console_cmd_register(&sd_cmd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commande sd : %s", esp_err_to_name(err));
        return err;
    }

    return esp_console_start_repl(repl);
}
