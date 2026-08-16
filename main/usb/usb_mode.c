#include "usb/usb_mode.h"

#include "esp_log.h"

#include "usb/mode_otp.h"
#include "usb/mode_pgp.h"
#include "usb/mode_storage.h"
#include "usb/msc_disk.h"
#include "usb/usb_device.h"
#include "usb/usb_mode_state.h"

static const char *TAG = "usb_mode";

/* Valeur initiale d'une statique : USB_MODE_NONE, sans qu'il y ait besoin de
 * l'écrire — voir usb_mode.h sur pourquoi c'est délibéré. */
static usb_mode_t s_mode;

/*
 * s_mode décrit-il vraiment ce qui est installé ?
 *
 * Faux dès qu'une bascule échoue en cours de route : la pile a été démontée,
 * et ce qui devait la remplacer n'est pas en place. Sans ce drapeau, le
 * court-circuit « mode déjà courant » plus bas renverrait ESP_OK sans rien
 * installer, et le coffre resterait sans fonction USB en prétendant l'avoir.
 * Une bascule vers le même mode doit donc rester possible tant que l'état
 * n'est pas certain. Vrai après usb_mode_init() : rien n'est installé, et
 * c'est exactement ce que dit USB_MODE_NONE.
 */
static bool s_mode_known;

esp_err_t usb_mode_init(void)
{
    /* Rien à installer : au démarrage rien n'est encore branché côté
     * matériel, donc il n'y a rien à désinstaller non plus. */
    s_mode = USB_MODE_NONE;
    s_mode_known = true;
    return ESP_OK;
}

usb_mode_t usb_mode_get(void)
{
    return s_mode;
}

bool usb_mode_is_known(void)
{
    return s_mode_known;
}

esp_err_t usb_mode_set(usb_mode_t mode)
{
    if (mode == s_mode && s_mode_known) {
        return ESP_OK;
    }

    if (mode >= USB_MODE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "mode USB : %s -> %s", usb_mode_name(s_mode), usb_mode_name(mode));

    /*
     * Le worker CCID d'abord, la pile USB ensuite. Il survit à tous les
     * démontages (créé une fois, jamais détruit) et poste des callbacks sur la
     * file de tud_task pendant qu'il attend une confirmation physique —
     * jusqu'à 15 s. tud_deinit() détruit cette file : le laisser tourner
     * pendant la désinstallation, c'est écrire dans une file libérée. Voir la
     * divergence BLOQUANT 1 en tête de main/security/ccid.c.
     */
    if (s_mode == USB_MODE_PGP) {
        mode_pgp_stop();
    }

    /* À partir d'ici la pile est démontée ou en train de l'être : plus rien
     * n'est garanti tant qu'une installation n'a pas réussi. */
    s_mode_known = false;

    /*
     * Toujours désinstaller avant de réinstaller, même vers USB_MODE_NONE :
     * c'est le seul moyen honnête de faire voir à l'hôte une vraie
     * déconnexion avant la nouvelle énumération (ou l'absence de). C'est
     * aussi ce qui garantit que le stockage et le CCID ne coexistent
     * jamais : à tout instant, au plus un jeu de descripteurs est installé.
     */
    esp_err_t err = usb_device_uninstall();
    if (err != ESP_OK) {
        /*
         * Échec de la désinstallation : elle n'a alors RIEN détaché — ni
         * tud_disconnect(), ni tusb_deinit(), ni le PHY (contrat en tête de
         * usb_device.h). Le périphérique du mode PRÉCÉDENT est donc toujours
         * sur le bus, et c'est lui qu'il faut annoncer : poser USB_MODE_NONE
         * ici était la résurgence, par une autre porte, du s_mode qui ment.
         *
         * Conserver le mode n'est pas cosmétique — c'est ce qui fait repasser
         * la tentative suivante par le mode_pgp_stop() ci-dessus, donc referme
         * la porte des callbacks CCID AVANT le tusb_deinit() qui détruit la
         * file de tud_task (divergence BLOQUANT 1 en tête de
         * security/ccid.c). Avec NONE, cette étape était sautée et la sûreté
         * ne tenait plus que par la rémanence de s_shutdown.
         *
         * Et on NE réarme PAS le CCID au passage, malgré l'apparente
         * asymétrie avec mode_pgp_stop() : usb_device_uninstall() a déjà posé
         * son drapeau d'arrêt de tâche avant d'échouer, donc plus personne ne
         * fera tourner tud_task. Un CCID rouvert n'aurait rien pour répondre,
         * et son premier usbd_defer_func() irait dans une file que plus
         * personne ne vide — osal_queue_send() y attend sans limite de temps
         * (managed_components/espressif__tinyusb/src/osal/osal_freertos.h:282).
         * Le mode est fini ; la reprise passe par une nouvelle bascule, pas
         * par une réanimation.
         */
        ESP_LOGE(TAG, "désinstallation avant bascule : %s", esp_err_to_name(err));
        const usb_mode_state_t st = usb_mode_state_on_failure(s_mode, USB_MODE_FAIL_UNINSTALL);
        s_mode = st.mode;
        s_mode_known = st.known;
        return err;
    }

    if (mode == USB_MODE_NONE) {
        s_mode = USB_MODE_NONE;
        s_mode_known = true;
        return ESP_OK;
    }

    int string_count = 0;
    const char **strings;
    const uint8_t *fs_cfg;
    const uint8_t *hs_cfg;

    if (mode == USB_MODE_STORAGE) {
        err = msc_disk_init();
        if (err != ESP_OK) {
            /* La désinstallation, elle, a réussi : plus aucune fonction n'est
             * exposée. Laisser s_mode sur l'ancienne valeur ferait mentir
             * usb_mode_get() ET court-circuiter la prochaine demande de ce
             * même mode, qui renverrait ESP_OK sans rien installer. */
            ESP_LOGE(TAG, "init du disque : %s", esp_err_to_name(err));
            const usb_mode_state_t st =
                usb_mode_state_on_failure(s_mode, USB_MODE_FAIL_AFTER_UNINSTALL);
            s_mode = st.mode;
            s_mode_known = st.known;
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
        /* Même raison qu'au-dessus : rien n'est installé, il faut le dire. */
        ESP_LOGE(TAG, "installation du mode %s : %s",
                 usb_mode_name(mode), esp_err_to_name(err));
        const usb_mode_state_t st =
            usb_mode_state_on_failure(s_mode, USB_MODE_FAIL_AFTER_UNINSTALL);
        s_mode = st.mode;
        s_mode_known = st.known;
        return err;
    }

    if (mode == USB_MODE_PGP) {
        /*
         * Après coup, jamais avant : usb_device_install() vient de faire
         * tourner ccid_drv_init() (via tusb_init()), qui réarme les PIN
         * d'usine en RAM. Charger l'état persisté plus tôt se ferait
         * écraser. Voir le commentaire de mode_pgp_data_load() pour le choix
         * « à l'entrée du mode » plutôt qu'au démarrage (tâche 12).
         */
        mode_pgp_data_load();
    }

    s_mode = mode;
    s_mode_known = true;
    return ESP_OK;
}
