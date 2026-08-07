#include "usb/usb_mode.h"

#include "esp_log.h"

#include "usb/mode_otp.h"
#include "usb/mode_pgp.h"
#include "usb/mode_storage.h"
#include "usb/msc_disk.h"
#include "usb/usb_device.h"

static const char *TAG = "usb_mode";

/* Valeur initiale d'une statique : USB_MODE_NONE, sans qu'il y ait besoin de
 * l'écrire — voir usb_mode.h sur pourquoi c'est délibéré. */
static usb_mode_t s_mode;

esp_err_t usb_mode_init(void)
{
    /* Rien à installer : au démarrage rien n'est encore branché côté
     * matériel, donc il n'y a rien à désinstaller non plus. */
    s_mode = USB_MODE_NONE;
    return ESP_OK;
}

usb_mode_t usb_mode_get(void)
{
    return s_mode;
}

esp_err_t usb_mode_set(usb_mode_t mode)
{
    if (mode == s_mode) {
        return ESP_OK;
    }

    if (mode >= USB_MODE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "mode USB : %s -> %s", usb_mode_name(s_mode), usb_mode_name(mode));

    /*
     * Toujours désinstaller avant de réinstaller, même vers USB_MODE_NONE :
     * c'est le seul moyen honnête de faire voir à l'hôte une vraie
     * déconnexion avant la nouvelle énumération (ou l'absence de). C'est
     * aussi ce qui garantit que le stockage et le CCID ne coexistent
     * jamais : à tout instant, au plus un jeu de descripteurs est installé.
     */
    esp_err_t err = usb_device_uninstall();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "désinstallation avant bascule : %s", esp_err_to_name(err));
        return err;
    }

    if (mode == USB_MODE_NONE) {
        s_mode = USB_MODE_NONE;
        return ESP_OK;
    }

    int string_count = 0;
    const char **strings;
    const uint8_t *fs_cfg;
    const uint8_t *hs_cfg;

    if (mode == USB_MODE_STORAGE) {
        err = msc_disk_init();
        if (err != ESP_OK) {
            return err;
        }
        strings = mode_storage_strings(&string_count);
        fs_cfg = mode_storage_fs_config();
        hs_cfg = mode_storage_hs_config();
    } else if (mode == USB_MODE_PGP) {
        /* USB_MODE_PGP : la carte OpenPGP sur CCID (tâche 10). Rien à
         * initialiser ici — mode_pgp_fs_config()/hs_config() force le
         * démarrage du worker CCID au passage, voir mode_pgp.c. */
        strings = mode_pgp_strings(&string_count);
        fs_cfg = mode_pgp_fs_config();
        hs_cfg = mode_pgp_hs_config();
    } else {
        /* USB_MODE_OTP : la clé CR-HMAC sur HID (tâche 11). Rien à
         * initialiser ici non plus — mode_otp_fs_config()/hs_config() câble
         * les hooks otp_proto au passage, voir mode_otp.c. */
        strings = mode_otp_strings(&string_count);
        fs_cfg = mode_otp_fs_config();
        hs_cfg = mode_otp_hs_config();
    }

    err = usb_device_install(fs_cfg, hs_cfg, strings, string_count);
    if (err != ESP_OK) {
        return err;
    }

    s_mode = mode;
    return ESP_OK;
}
