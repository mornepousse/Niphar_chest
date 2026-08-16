#include "console/console.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdmmc_cmd.h"

#include "sec_gate.h"
#include "storage/sd_card.h"
#include "usb/msc_disk.h"
#include "usb/usb_device.h"
#include "usb/usb_mode.h"

static const char *TAG = "console";

#if BOARD_HAS_SD
static const char *pwr_path_str(sd_pwr_path_t p)
{
    switch (p) {
    case SD_PWR_EXTERNAL:    return "externe (VDDPST_5 en 3,3 V)";
    case SD_PWR_ON_CHIP_LDO: return "LDO interne canal 4 (LDO_VO4)";
    default:                 return "inconnu";
    }
}

/*
 * Débit SDMMC brut, sans l'USB dans la boucle.
 *
 * C'est la seule mesure qui sépare « la carte est lente » de « mon chemin MSC
 * est lent » : elle lit de gros blocs alignés dans un tampon DMA, exactement ce
 * que le chemin rapide de msc_disk est censé faire. Lecture seule — rien n'est
 * écrit sur la carte.
 */
static int cmd_sd_bench(void)
{
    if (!sd_present()) {
        printf("aucune carte.\n");
        return 1;
    }

    const uint32_t ss = sd_sector_size();
    const uint32_t chunk_sectors = 128;              /* 64 Kio par transfert */
    const uint32_t total_bytes = 8u * 1024 * 1024;   /* 8 Mio */
    const uint32_t chunks = total_bytes / (chunk_sectors * ss);

    uint8_t *buf = heap_caps_malloc(chunk_sectors * ss, MALLOC_CAP_DMA);
    if (buf == NULL) {
        printf("mémoire DMA insuffisante pour %" PRIu32 " o.\n", chunk_sectors * ss);
        return 1;
    }

    printf("lecture de %" PRIu32 " Mio par blocs de %" PRIu32 " Kio…\n",
           total_bytes / (1024 * 1024), (chunk_sectors * ss) / 1024);

    const int64_t t0 = esp_timer_get_time();
    for (uint32_t i = 0; i < chunks; i++) {
        esp_err_t err = sd_read_sectors(buf, i * chunk_sectors, chunk_sectors);
        if (err != ESP_OK) {
            printf("échec au bloc %" PRIu32 " : %s\n", i, esp_err_to_name(err));
            free(buf);
            return 1;
        }
    }
    const int64_t dt = esp_timer_get_time() - t0;
    free(buf);

    if (dt <= 0) {
        printf("mesure invalide.\n");
        return 1;
    }
    printf("SDMMC brut : %.1f Mio/s (%lld ms)\n",
           (double)total_bytes / (double)dt, (long long)(dt / 1000));
    return 0;
}

static int cmd_sd(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage : sd info | sd probe | sd bench\n");
        return 1;
    }

    if (strcmp(argv[1], "bench") == 0) {
        return cmd_sd_bench();
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
#endif /* BOARD_HAS_SD */

static int cmd_usb(int argc, char **argv)
{
    (void)argc;
    (void)argv;

#if BOARD_CONSOLE_ACTIONS
    /*
     * Béquille de développement : sur une carte avec lien, le sélecteur de
     * mode viendra du clavier via le S3, pas de la console. Verrouillée
     * comme sec_gate_console_confirm — voir le garde-fou 4 de fast.sh, étendu
     * à usb_mode_set par cette même tâche.
     */
    if (argc >= 3 && strcmp(argv[1], "mode") == 0) {
        usb_mode_t m = USB_MODE_NONE;
        if      (strcmp(argv[2], "none")    == 0) m = USB_MODE_NONE;
        else if (strcmp(argv[2], "storage") == 0) m = USB_MODE_STORAGE;
        else if (strcmp(argv[2], "pgp")     == 0) m = USB_MODE_PGP;
        else if (strcmp(argv[2], "otp")     == 0) m = USB_MODE_OTP;
        else { printf("mode inconnu : %s\n", argv[2]); return 1; }
        esp_err_t err = usb_mode_set(m);
        printf("mode %s : %s\n", usb_mode_name(m), esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
#endif

    msc_disk_stats_t st;
    msc_disk_get_stats(&st);

    /* L'incertitude s'affiche : après une bascule ratée, le mode annoncé est
     * ce que l'hôte voit encore, pas ce qui est servi. Sans cette mention, la
     * console montrerait un mode d'aplomb dans le seul cas où il ne faut pas
     * s'y fier — voir usb/usb_mode_state.h. */
    printf("mode          : %s%s\n", usb_mode_name(usb_mode_get()),
           usb_mode_is_known() ? "" : "  (INCERTAIN — dernière bascule en échec, rebasculer)");
    printf("état          : %s\n", usb_device_mounted() ? "configuré par l'hôte" : "non configuré");
    printf("bufsize max   : %" PRIu32 " o (demandé par TinyUSB)\n", st.max_bufsize);
    printf("secteurs DMA  : %" PRIu32 "\n", st.fast_sectors);
    printf("secteurs rebond: %" PRIu32 "\n", st.bounce_sectors);
    printf("tampons non-DMA: %" PRIu32 "\n", st.not_dma_hits);

    const uint32_t total = st.fast_sectors + st.bounce_sectors;
    if (total > 0) {
        printf("part rebond   : %" PRIu32 " %%\n", (st.bounce_sectors * 100) / total);
        if (st.bounce_sectors > st.fast_sectors) {
            printf("→ le rebond domine : chaque secteur coûte une transaction SDMMC.\n");
        }
    } else {
        printf("(aucun transfert encore)\n");
    }
    return 0;
}

static int cmd_sec(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage : sec confirm | sec source\n");
        return 1;
    }

    if (strcmp(argv[1], "source") == 0) {
        printf("confirmation : %s\n", sec_gate_source());
        return 0;
    }

#if BOARD_CONSOLE_ACTIONS
    if (strcmp(argv[1], "confirm") == 0) {
        sec_gate_console_confirm();
        printf("appui simulé — sans effet s'il n'y a pas d'opération armée.\n");
        return 0;
    }
#endif

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

#if BOARD_HAS_SD
    const esp_console_cmd_t sd_cmd = {
        .command = "sd",
        .help = "Carte microSD : « sd info », « sd probe », « sd bench » (débit brut, lecture seule)",
        .hint = "info|probe|bench",
        .func = &cmd_sd,
    };
    err = esp_console_cmd_register(&sd_cmd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commande sd : %s", esp_err_to_name(err));
        return err;
    }
#endif /* BOARD_HAS_SD */

    const esp_console_cmd_t usb_cmd = {
        .command = "usb",
        .help = "État USB, compteurs du chemin MSC (rapide vs rebond)"
#if BOARD_CONSOLE_ACTIONS
                ", et « usb mode none|storage|pgp|otp » (béquille de dev, sans lien)"
#endif
                ,
        .hint = NULL,
        .func = &cmd_usb,
    };
    err = esp_console_cmd_register(&usb_cmd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commande usb : %s", esp_err_to_name(err));
        return err;
    }

    const esp_console_cmd_t sec_cmd = {
        .command = "sec",
        .help = "Sécurité : « sec source », et « sec confirm » sur carte sans lien",
        .hint = "confirm|source",
        .func = &cmd_sec,
    };
    err = esp_console_cmd_register(&sec_cmd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commande sec : %s", esp_err_to_name(err));
        return err;
    }

    return esp_console_start_repl(repl);
}
