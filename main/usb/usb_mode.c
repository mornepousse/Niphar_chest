#include "usb/usb_mode.h"

#include "esp_log.h"

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

    if (mode == USB_MODE_PGP || mode == USB_MODE_OTP) {
        /* Descripteurs pas encore écrits — tâches 10 et 11. */
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (mode >= USB_MODE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "mode USB : %s -> %s", usb_mode_name(s_mode), usb_mode_name(mode));

    /*
     * Toujours désinstaller avant de réinstaller, même vers USB_MODE_NONE :
     * c'est le seul moyen honnête de faire voir à l'hôte une vraie
     * déconnexion avant la nouvelle énumération (ou l'absence de).
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

    /* USB_MODE_STORAGE : le seul mode installable pour l'instant. */
    err = msc_disk_init();
    if (err != ESP_OK) {
        return err;
    }

    int string_count = 0;
    const char **strings = mode_storage_strings(&string_count);
    err = usb_device_install(mode_storage_fs_config(), mode_storage_hs_config(),
                              strings, string_count);
    if (err != ESP_OK) {
        return err;
    }

    s_mode = USB_MODE_STORAGE;
    return ESP_OK;
}
