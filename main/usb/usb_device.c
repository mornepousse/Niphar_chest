#include "usb/usb_device.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tusb.h"

static const char *TAG = "usb";

/*
 * VID/PID.
 *
 * 0x303A est le Vendor ID d'Espressif. Le PID est choisi distinct de celui de
 * KeSp_firmware pour que les règles udev et les clients de l'hôte ne confondent
 * jamais le coffre avec le clavier ou son dongle. À faire enregistrer auprès
 * d'Espressif si le projet sort un jour du domaine privé.
 *
 * Un seul PID pour tous les modes : le coffre n'expose jamais deux fonctions
 * ensemble (usb_mode.h), donc l'hôte ne voit jamais deux identités à la fois.
 */
#define NIPHAR_USB_VID 0x303A
#define NIPHAR_USB_PID 0x4021

/*
 * Index de chaîne communs à tous les modes. Chaque table de chaînes fournie à
 * usb_device_install() doit les respecter dans cet ordre : c'est la seule
 * convention qui relie ce fichier (qui ne connaît aucun mode) aux indices
 * iManufacturer/iProduct/iSerialNumber figés dans desc_device ci-dessous.
 */
#define USB_STRID_LANGID       0
#define USB_STRID_MANUFACTURER 1
#define USB_STRID_PRODUCT      2
#define USB_STRID_SERIAL       3

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,   /* classe portée par l'interface */
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = NIPHAR_USB_VID,
    .idProduct          = NIPHAR_USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = USB_STRID_MANUFACTURER,
    .iProduct           = USB_STRID_PRODUCT,
    .iSerialNumber      = USB_STRID_SERIAL,
    .bNumConfigurations = 0x01,
};

/*
 * Le device_qualifier décrit l'autre vitesse. Un périphérique haute vitesse qui
 * ne le fournit pas se fait rétrograder par certains hôtes.
 */
static const tusb_desc_device_qualifier_t desc_qualifier = {
    .bLength            = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType    = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved          = 0x00,
};

/*
 * Numéro de série dérivé de la MAC. Un hôte identifie un volume de stockage par
 * son numéro de série : le figer ferait passer deux coffres pour le même
 * périphérique, avec des règles udev et un cache d'hôte qui se mélangent.
 *
 * Partagé par tous les modes (voir usb_device_serial() ci-dessous) : c'est
 * une identité de l'appareil, pas d'une fonction.
 */
static char s_serial[13] = "000000000000";
static bool s_serial_ready;

/* Descripteurs et chaînes du mode actuellement installé — fournis par
 * usb_device_install(), NULL quand rien n'est installé. */
static const uint8_t *s_fs_cfg;
static const uint8_t *s_hs_cfg;
static const char **s_strings;
static int s_string_count;

static usb_phy_handle_t s_phy;
static TaskHandle_t s_task;
static volatile bool s_task_run;
static SemaphoreHandle_t s_task_done;
static volatile bool s_mounted;

const char *usb_device_serial(void)
{
    if (!s_serial_ready) {
        uint8_t mac[6] = { 0 };
        if (esp_efuse_mac_get_default(mac) == ESP_OK) {
            snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
        s_serial_ready = true;
    }
    return s_serial;
}

/* ------------------------------------------------------------------------ */
/* Callbacks de descripteurs                                                 */
/* ------------------------------------------------------------------------ */

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return (tud_speed_get() == TUSB_SPEED_HIGH) ? s_hs_cfg : s_fs_cfg;
}

uint8_t const *tud_descriptor_device_qualifier_cb(void)
{
    return (uint8_t const *)&desc_qualifier;
}

/*
 * Descripteur « other speed » : la même configuration, vue à l'autre vitesse.
 * Le type doit être réécrit, d'où la copie — le descripteur rendu doit rester
 * valide après le retour, c'est pourquoi le tampon est statique.
 *
 * La longueur totale n'est plus une constante de compilation puisque le
 * contenu dépend du mode installé : elle se lit dans l'en-tête du descripteur
 * de configuration source (wTotalLength, octets 2-3, petit-boutien). Le
 * tampon est dimensionné large pour couvrir les modes futurs (CCID, HID).
 */
#define OTHER_SPEED_CFG_MAX 256

uint8_t const *tud_descriptor_other_speed_configuration_cb(uint8_t index)
{
    (void)index;
    static uint8_t buf[OTHER_SPEED_CFG_MAX];

    const uint8_t *src = (tud_speed_get() == TUSB_SPEED_HIGH) ? s_fs_cfg : s_hs_cfg;
    if (src == NULL) {
        return NULL;
    }

    uint16_t len = (uint16_t)(src[2] | ((uint16_t)src[3] << 8));
    if (len > sizeof(buf)) {
        /* Ne devrait jamais arriver avec les descripteurs du projet — mais
         * tronquer plutôt que déborder si un mode futur grossit trop. */
        ESP_LOGE(TAG, "descripteur other-speed tronqué : %u > %u o",
                 (unsigned)len, (unsigned)sizeof(buf));
        len = sizeof(buf);
    }
    memcpy(buf, src, len);
    buf[1] = TUSB_DESC_OTHER_SPEED_CONFIG;
    /*
     * wTotalLength doit décrire le tampon RENDU, pas la source. Borner le
     * memcpy ne suffit pas : TinyUSB relit ce champ dans le tampon qu'on lui
     * rend et transfère cette longueur-là (usbd.c, process_get_descriptor).
     * Avec la valeur source laissée en place, une troncature ferait lire
     * au-delà des OTHER_SPEED_CFG_MAX octets de buf et enverrait le
     * débordement de .bss à l'hôte. Inatteignable avec les modes actuels
     * (86 octets au plus) — c'est justement pourquoi ça doit être écrit ici et
     * pas le jour où un mode grossira.
     */
    buf[2] = (uint8_t)(len & 0xFF);
    buf[3] = (uint8_t)((len >> 8) & 0xFF);
    return buf;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t desc[32];

    if (s_strings == NULL || index >= (uint8_t)s_string_count || s_strings[index] == NULL) {
        return NULL;
    }

    uint8_t count;
    if (index == USB_STRID_LANGID) {
        memcpy(&desc[1], s_strings[USB_STRID_LANGID], 2);
        count = 1;
    } else {
        const char *str = s_strings[index];
        const size_t max = (sizeof(desc) / sizeof(desc[0])) - 1;
        size_t len = strlen(str);
        if (len > max) {
            len = max;   /* tronqué plutôt que débordé */
        }
        for (size_t i = 0; i < len; i++) {
            /* Le double cast n'est pas décoratif : `char` est signé sur RISC-V,
             * et un octet ≥ 0x80 deviendrait 0xFFxx en UTF-16 par extension de
             * signe. Inoffensif tant que les chaînes sont ASCII, silencieux le
             * jour où l'une sera accentuée. */
            desc[i + 1] = (uint16_t)(uint8_t)str[i];   /* vers UTF-16LE */
        }
        count = (uint8_t)len;
    }

    /* En-tête : longueur totale en octets, puis le type. */
    desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * count + 2));
    return desc;
}

/* ------------------------------------------------------------------------ */
/* Événements de l'hôte                                                      */
/* ------------------------------------------------------------------------ */

void tud_mount_cb(void)
{
    s_mounted = true;
    ESP_LOGI(TAG, "configuré par l'hôte (%s)",
             tud_speed_get() == TUSB_SPEED_HIGH ? "haute vitesse" : "pleine vitesse");
}

void tud_umount_cb(void)
{
    s_mounted = false;
    ESP_LOGI(TAG, "hôte déconnecté");
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    /*
     * Suspendu, mais JAMAIS de deep-sleep : l'USB-Serial-JTAG est l'unique voie
     * de reflash du coffre et il doit rester vivant. Voir docs/HARDWARE.md.
     */
    ESP_LOGD(TAG, "bus suspendu");
}

void tud_resume_cb(void)
{
    ESP_LOGD(TAG, "bus repris");
}

/* ------------------------------------------------------------------------ */
/* Installation / désinstallation                                            */
/* ------------------------------------------------------------------------ */

static void usb_task(void *arg)
{
    (void)arg;
    while (s_task_run) {
        /* Timeout court plutôt qu'un blocage indéfini : c'est ce qui permet à
         * usb_device_uninstall() de faire sortir proprement cette boucle sans
         * toucher à l'état interne de TinyUSB depuis un autre contexte. */
        tud_task_ext(100, false);
    }
    xSemaphoreGive(s_task_done);
    vTaskDelete(NULL);
}

esp_err_t usb_device_install(const uint8_t *fs_cfg, const uint8_t *hs_cfg,
                              const char **strings, int string_count)
{
    if (fs_cfg == NULL || hs_cfg == NULL || strings == NULL || string_count <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_phy != NULL) {
        ESP_LOGE(TAG, "installation demandée alors qu'un mode est déjà en place");
        return ESP_ERR_INVALID_STATE;
    }

    s_fs_cfg = fs_cfg;
    s_hs_cfg = hs_cfg;
    s_strings = strings;
    s_string_count = string_count;

    /*
     * PHY UTMI interne, en mode device. Pas de monitoring VBUS : le coffre est
     * alimenté par le bus, donc la présence de l'hôte est acquise dès qu'il
     * tourne.
     */
    const usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_UTMI,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_UNDEFINED,
        .ext_io_conf = NULL,
        .otg_io_conf = NULL,
    };
    esp_err_t err = usb_new_phy(&phy_conf, &s_phy);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PHY USB : %s", esp_err_to_name(err));
        s_fs_cfg = NULL;
        s_hs_cfg = NULL;
        s_strings = NULL;
        s_string_count = 0;
        return err;
    }

    if (!tusb_init()) {
        ESP_LOGE(TAG, "tusb_init a échoué");
        usb_del_phy(s_phy);
        s_phy = NULL;
        s_fs_cfg = NULL;
        s_hs_cfg = NULL;
        s_strings = NULL;
        s_string_count = 0;
        return ESP_FAIL;
    }

    if (s_task_done == NULL) {
        s_task_done = xSemaphoreCreateBinary();
        if (s_task_done == NULL) {
            tusb_deinit(TUD_OPT_RHPORT);
            usb_del_phy(s_phy);
            s_phy = NULL;
            s_fs_cfg = NULL;
            s_hs_cfg = NULL;
            s_strings = NULL;
            s_string_count = 0;
            return ESP_ERR_NO_MEM;
        }
    }

    /*
     * Priorité au-dessus de la boucle applicative : un NAK tardif sur un
     * transfert de masse dégrade le débit, et l'hôte a des délais serrés.
     */
    s_task_run = true;
    if (xTaskCreate(usb_task, "usb", 4096, NULL, 5, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "création de la tâche USB");
        s_task_run = false;
        tusb_deinit(TUD_OPT_RHPORT);
        usb_del_phy(s_phy);
        s_phy = NULL;
        s_fs_cfg = NULL;
        s_hs_cfg = NULL;
        s_strings = NULL;
        s_string_count = 0;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "périphérique installé, %04x:%04x, série %s",
             NIPHAR_USB_VID, NIPHAR_USB_PID, usb_device_serial());
    return ESP_OK;
}

esp_err_t usb_device_uninstall(void)
{
    if (s_phy == NULL && s_task == NULL) {
        return ESP_OK;   /* déjà désinstallé — idempotent, voir usb_device.h */
    }

    /*
     * La tâche USB s'arrête AVANT tout le reste.
     *
     * usbd n'est pas réentrant, et tud_disconnect() comme tusb_deinit()
     * touchent les mêmes registres DWC2 que tud_task_ext(). Les appeler depuis
     * la tâche appelante pendant que usb_task tourne encore, c'est deux
     * contextes dans la même machine à états. L'ordre est donc : sortir la
     * boucle, attendre qu'elle ait vraiment rendu la main, puis détacher.
     */
    if (s_task != NULL) {
        /*
         * Purge du sémaphore avant d'attendre : il est binaire et n'est jamais
         * consommé quand un `give` arrive après l'expiration du take. Ce jeton
         * resté en réserve pré-armerait la désinstallation SUIVANTE, qui
         * rendrait la main immédiatement alors que sa tâche n'est pas sortie.
         */
        if (s_task_done != NULL) {
            (void)xSemaphoreTake(s_task_done, 0);
        }

        s_task_run = false;

        /* Marge large devant le tud_task_ext(100) de la boucle. */
        BaseType_t done = pdFALSE;
        if (s_task_done != NULL) {
            done = xSemaphoreTake(s_task_done, pdMS_TO_TICKS(1000));
        }
        if (done != pdTRUE) {
            /*
             * Ne PAS enchaîner : tusb_deinit() et usb_del_phy() sous une tâche
             * encore vivante, c'est une utilisation après libération. On rend
             * l'échec, s_task reste posé — une nouvelle tentative réattendra le
             * sémaphore, que la tâche finira par donner en sortant.
             */
            ESP_LOGE(TAG, "la tâche USB n'est pas sortie en 1 s — désinstallation refusée");
            return ESP_ERR_TIMEOUT;
        }
        s_task = NULL;
    }

    /* Détache du bus : l'hôte doit voir une vraie déconnexion, pas un silence
     * qu'il pourrait attribuer à un périphérique figé. Plus personne ne tourne
     * dans tud_task à ce point. */
    if (tusb_inited()) {
        tud_disconnect();
    }

    tusb_deinit(TUD_OPT_RHPORT);

    if (s_phy != NULL) {
        usb_del_phy(s_phy);
        s_phy = NULL;
    }

    s_fs_cfg = NULL;
    s_hs_cfg = NULL;
    s_strings = NULL;
    s_string_count = 0;
    s_mounted = false;

    ESP_LOGI(TAG, "périphérique désinstallé");
    return ESP_OK;
}

bool usb_device_mounted(void)
{
    return s_mounted;
}
