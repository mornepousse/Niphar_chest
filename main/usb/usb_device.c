#include "usb/usb_device.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"

#include "usb/msc_disk.h"

static const char *TAG = "usb";

/*
 * VID/PID.
 *
 * 0x303A est le Vendor ID d'Espressif. Le PID est choisi distinct de celui de
 * KeSp_firmware pour que les règles udev et les clients de l'hôte ne confondent
 * jamais le coffre avec le clavier ou son dongle. À faire enregistrer auprès
 * d'Espressif si le projet sort un jour du domaine privé.
 */
#define NIPHAR_USB_VID 0x303A
#define NIPHAR_USB_PID 0x4021

enum {
    ITF_NUM_MSC = 0,
    ITF_NUM_TOTAL,
};

/* EP0 est réservé ; le MSC prend une paire bulk. */
#define EPNUM_MSC_OUT 0x01
#define EPNUM_MSC_IN  0x81

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_MSC,
    STRID_COUNT,
};

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
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
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
 * Bus-powered : le coffre n'a pas d'autre source que l'USB, d'où l'absence de
 * l'attribut self-powered et une consommation annoncée de 500 mA.
 */
static const uint8_t desc_fs_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 500),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, STRID_MSC, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

static const uint8_t desc_hs_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 500),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, STRID_MSC, EPNUM_MSC_OUT, EPNUM_MSC_IN, 512),
};

/*
 * Numéro de série dérivé de la MAC. Un hôte identifie un volume de stockage par
 * son numéro de série : le figer ferait passer deux coffres pour le même
 * périphérique, avec des règles udev et un cache d'hôte qui se mélangent.
 */
static char s_serial[13] = "000000000000";

static const char *s_strings[STRID_COUNT] = {
    [STRID_LANGID]       = (const char[]){ 0x09, 0x04 },  /* anglais (US) */
    [STRID_MANUFACTURER] = "Mae PUGIN",
    [STRID_PRODUCT]      = "Coffre Niphar",
    [STRID_SERIAL]       = s_serial,
    [STRID_MSC]          = "Coffre microSD",
};

static usb_phy_handle_t s_phy;
static TaskHandle_t s_task;
static volatile bool s_mounted;

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
    return (tud_speed_get() == TUSB_SPEED_HIGH) ? desc_hs_config : desc_fs_config;
}

uint8_t const *tud_descriptor_device_qualifier_cb(void)
{
    return (uint8_t const *)&desc_qualifier;
}

/*
 * Descripteur « other speed » : la même configuration, vue à l'autre vitesse.
 * Le type doit être réécrit, d'où la copie — le descripteur rendu doit rester
 * valide après le retour, c'est pourquoi le tampon est statique.
 */
uint8_t const *tud_descriptor_other_speed_configuration_cb(uint8_t index)
{
    (void)index;
    static uint8_t buf[CONFIG_TOTAL_LEN];
    const uint8_t *src = (tud_speed_get() == TUSB_SPEED_HIGH) ? desc_fs_config : desc_hs_config;
    memcpy(buf, src, CONFIG_TOTAL_LEN);
    buf[1] = TUSB_DESC_OTHER_SPEED_CONFIG;
    return buf;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t desc[32];

    if (index >= STRID_COUNT || s_strings[index] == NULL) {
        return NULL;
    }

    uint8_t count;
    if (index == STRID_LANGID) {
        memcpy(&desc[1], s_strings[STRID_LANGID], 2);
        count = 1;
    } else {
        const char *str = s_strings[index];
        const size_t max = (sizeof(desc) / sizeof(desc[0])) - 1;
        size_t len = strlen(str);
        if (len > max) {
            len = max;   /* tronqué plutôt que débordé */
        }
        for (size_t i = 0; i < len; i++) {
            desc[i + 1] = (uint16_t)str[i];   /* ASCII vers UTF-16LE */
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
/* Installation                                                              */
/* ------------------------------------------------------------------------ */

static void usb_task(void *arg)
{
    (void)arg;
    while (true) {
        tud_task();   /* bloquant tant qu'il n'y a rien à faire */
    }
}

esp_err_t usb_device_start(void)
{
    uint8_t mac[6] = { 0 };
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    esp_err_t err = msc_disk_init();
    if (err != ESP_OK) {
        return err;
    }

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
    ESP_RETURN_ON_ERROR(usb_new_phy(&phy_conf, &s_phy), TAG, "PHY USB");

    if (!tusb_init()) {
        ESP_LOGE(TAG, "tusb_init a échoué");
        usb_del_phy(s_phy);
        s_phy = NULL;
        return ESP_FAIL;
    }

    /*
     * Priorité au-dessus de la boucle applicative : un NAK tardif sur un
     * transfert de masse dégrade le débit, et l'hôte a des délais serrés.
     */
    if (xTaskCreate(usb_task, "usb", 4096, NULL, 5, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "création de la tâche USB");
        usb_del_phy(s_phy);
        s_phy = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "périphérique MSC sur le port haute vitesse, %04x:%04x, série %s",
             NIPHAR_USB_VID, NIPHAR_USB_PID, s_serial);
    return ESP_OK;
}

bool usb_device_mounted(void)
{
    return s_mounted;
}
