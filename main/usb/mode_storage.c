#include "usb/mode_storage.h"

#include "tusb.h"

#include "usb/usb_device.h"

/*
 * Anciennement dans usb_device.c, avant que celui-ci ne devienne agnostique
 * du mode (tâche 9 — voir usb/usb_mode.h). Les callbacks tud_msc_* restent
 * dans msc_disk.c, qui ne dépend pas non plus du mode.
 */

enum {
    ITF_NUM_MSC = 0,
    ITF_NUM_TOTAL,
};

/* EP0 est réservé ; le MSC prend une paire bulk. */
#define EPNUM_MSC_OUT 0x01
#define EPNUM_MSC_IN  0x81

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

/*
 * Les trois premiers index (LANGID/MANUFACTURER/PRODUCT/SERIAL) suivent la
 * convention fixée par usb_device.c — voir USB_STRID_* dans usb_device.c et
 * les indices figés dans son desc_device. STRID_MSC est propre à ce mode.
 */
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_MSC,
    STRID_COUNT,
};

/*
 * Bus-powered : le coffre n'a pas d'autre source que l'USB, d'où l'absence de
 * l'attribut self-powered et une consommation annoncée de 500 mA.
 */
static const uint8_t s_fs_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 500),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, STRID_MSC, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

static const uint8_t s_hs_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 500),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, STRID_MSC, EPNUM_MSC_OUT, EPNUM_MSC_IN, 512),
};

static const uint8_t s_langid_bytes[2] = { 0x09, 0x04 };   /* anglais (US) */

/*
 * STRID_SERIAL vaut NULL ici : le numéro de série dépend de la MAC, connue
 * seulement à l'exécution. mode_storage_strings() le remplit à chaque appel.
 */
static const char *s_strings[STRID_COUNT] = {
    [STRID_LANGID]       = (const char *)s_langid_bytes,
    [STRID_MANUFACTURER] = "Mae PUGIN",
    [STRID_PRODUCT]      = "Coffre Niphar",
    [STRID_SERIAL]       = NULL,
    [STRID_MSC]          = "Coffre microSD",
};

const uint8_t *mode_storage_fs_config(void)
{
    return s_fs_config;
}

const uint8_t *mode_storage_hs_config(void)
{
    return s_hs_config;
}

const char **mode_storage_strings(int *out_count)
{
    s_strings[STRID_SERIAL] = usb_device_serial();
    if (out_count != NULL) {
        *out_count = STRID_COUNT;
    }
    return s_strings;
}
