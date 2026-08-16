#include "usb/mode_otp.h"

#include "tusb.h"

#include "otp_hid.h"
#include "otp_proto.h"
#include "usb/usb_device.h"

/*
 * Le mode OTP encapsule la clé CR-HMAC comme périphérique HID, sur le
 * protocole Yubikey OTP (feature reports de 8 octets échangés par
 * GET_REPORT/SET_REPORT sur EP0). Ce fichier ne connaît que les descripteurs,
 * les chaînes, et le report descriptor HID, sur le modèle exact de
 * mode_pgp.c — voir usb/mode_otp.h. La machine à états du protocole reste
 * dans security/otp_proto.c, et le câblage vers sec_store/sec_confirm/
 * cr_hmac dans security/otp_hid.c ; ni l'un ni l'autre ne dépend du mode.
 *
 * C'est ici que l'OTP-HID tourne pour la première fois sur du matériel :
 * chez KeSp_firmware, dont ce module est porté, il n'a jamais pu être validé
 * — l'ESP32-S3 n'avait pas assez d'endpoints IN pour le cumuler avec le
 * reste (CDC + HID clavier + OTP). Le coffre n'a pas cette contrainte : les
 * quatre modes USB sont mutuellement exclusifs au run-time (usb_mode.c), pas
 * au build.
 */

enum {
    ITF_NUM_HID = 0,
    ITF_NUM_TOTAL,
};

/* EP0 est réservé ; le HID prend une IN (interrupt), jamais utilisée pour de
 * vrai : le protocole OTP n'échange que des feature reports sur EP0
 * (GET_REPORT/SET_REPORT), l'endpoint interrupt n'existe que parce que la
 * classe HID en exige au moins une dans son interface descriptor. */
#define EPNUM_HID_IN 0x81

#define OTP_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

/*
 * Les trois premiers index (LANGID/MANUFACTURER/PRODUCT/SERIAL) suivent la
 * convention fixée par usb_device.c — voir USB_STRID_* dans usb_device.c et
 * mode_storage.c. STRID_HID est propre à ce mode.
 */
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_HID,
    STRID_COUNT,
};

/*
 * Report descriptor HID — collection application clavier (page 0x01, usage
 * 0x06) contenant un feature report vendor de 8 octets. Copié verbatim
 * depuis KeSp_firmware/main/comm/usb/usb_hid.c:103 (otp_hid_report_descriptor
 * là-bas) : les feature reports s'échangent sur EP0 via SET_REPORT/
 * GET_REPORT, indépendamment de l'usage page déclaré ici — c'est ce que fait
 * le protocole Yubikey OTP d'origine, et KeePassXC/ykpers le reconnaissent
 * tel quel.
 */
static const uint8_t desc_hid_report[] = {
    HID_USAGE_PAGE ( HID_USAGE_PAGE_DESKTOP            ),   /* 0x05, 0x01 */
    HID_USAGE      ( HID_USAGE_DESKTOP_KEYBOARD        ),   /* 0x09, 0x06 */
    HID_COLLECTION ( HID_COLLECTION_APPLICATION        ),   /* 0xA1, 0x01 */
        /* 8-byte vendor feature payload */
        HID_USAGE_PAGE_N ( HID_USAGE_PAGE_VENDOR, 2    ),   /* 0x06, 0x00, 0xFF */
        HID_USAGE        ( 0x01                        ),   /* 0x09, 0x01 */
        HID_LOGICAL_MIN  ( 0x00                        ),   /* 0x15, 0x00 */
        HID_LOGICAL_MAX_N( 0xFF, 2                     ),   /* 0x26, 0xFF, 0x00 */
        HID_REPORT_SIZE  ( 8                           ),   /* 0x75, 0x08 */
        HID_REPORT_COUNT ( 8                           ),   /* 0x95, 0x08 */
        HID_FEATURE      ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ), /* 0xB1, 0x02 */
    HID_COLLECTION_END                                      /* 0xC0 */
};

/*
 * Bus-powered, comme mode_storage et mode_pgp : le coffre n'a pas d'autre
 * source que l'USB.
 *
 * Les deux tableaux sont identiques : contrairement à TUD_MSC_DESCRIPTOR,
 * TUD_HID_DESCRIPTOR ne reçoit pas la taille de paquet en fonction de la
 * vitesse — 64 octets ici quelle que soit la vitesse (CFG_TUD_HID_EP_BUFSIZE
 * dans tusb_config.h), largement suffisant pour un feature report de 8
 * octets.
 */
static const uint8_t s_fs_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, OTP_CONFIG_TOTAL_LEN, 0x00, 500),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, STRID_HID, HID_ITF_PROTOCOL_NONE,
                       sizeof(desc_hid_report), EPNUM_HID_IN, 64, 10),
};

static const uint8_t s_hs_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, OTP_CONFIG_TOTAL_LEN, 0x00, 500),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, STRID_HID, HID_ITF_PROTOCOL_NONE,
                       sizeof(desc_hid_report), EPNUM_HID_IN, 64, 10),
};

static const uint8_t s_langid_bytes[2] = { 0x09, 0x04 };   /* anglais (US) */

/*
 * STRID_SERIAL vaut NULL ici : le numéro de série dépend de la MAC, connue
 * seulement à l'exécution. mode_otp_strings() le remplit à chaque appel.
 */
static const char *s_strings[STRID_COUNT] = {
    [STRID_LANGID]       = (const char *)s_langid_bytes,
    [STRID_MANUFACTURER] = "Mae PUGIN",
    [STRID_PRODUCT]      = "Coffre Niphar",
    [STRID_SERIAL]       = NULL,
    [STRID_HID]          = "Coffre OTP",
};

/* ------------------------------------------------------------------------ */
/* Callbacks TinyUSB de la classe HID                                        */
/* ------------------------------------------------------------------------ */

/*
 * Contrairement au CCID, HID est une vraie classe TinyUSB : ces callbacks
 * sont des symboles faibles de hid_device.c que TinyUSB appelle directement
 * une fois CFG_TUD_HID=1 (tusb_config.h) et l'interface enregistrée par
 * TUD_HID_DESCRIPTOR ci-dessus. Pas de force-link à la ccid_init() : aucun
 * pilote applicatif à tirer dans l'archive.
 *
 * Il n'y a qu'une seule interface HID installée à la fois (le coffre n'est
 * jamais clavier + OTP en même temps, à la différence du dongle KeSp d'où
 * vient ce fichier) : `instance` vaut toujours 0 et n'a pas besoin d'être
 * discriminé. Portées depuis
 * KeSp_firmware/main/comm/usb/usb_hid.c:237-290, en ne gardant que la
 * branche OTP — le clavier n'existe pas ici, et la branche LED (report
 * OUTPUT) avec elle : l'annoncer clavier (HID_ITF_PROTOCOL_KEYBOARD) aurait
 * fait que l'hôte lui envoie des rapports de LED, d'où HID_ITF_PROTOCOL_NONE
 * plus haut.
 */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return desc_hid_report;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)reqlen;
    if (report_type != HID_REPORT_TYPE_FEATURE) {
        return 0;
    }
    otp_proto_on_read(buffer);
    return OTP_FEATURE_RPT_SIZE;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    if (report_type != HID_REPORT_TYPE_FEATURE) {
        return;
    }
    if (bufsize >= OTP_FEATURE_RPT_SIZE) {
        otp_proto_on_write(buffer);
    }
}

/* ------------------------------------------------------------------------ */

/*
 * otp_hid_init() câble les hooks réels (sec_store/sec_confirm/cr_hmac) dans
 * otp_proto et remet la machine à états à ST_IDLE (otp_proto_reset()) —
 * appelé aux deux accesseurs comme ccid_init() dans mode_pgp.c, pour la même
 * raison : l'ordre d'évaluation des arguments de usb_device_install() n'est
 * pas spécifié en C, donc mode_otp_hs_config() peut être appelé avant
 * mode_otp_fs_config(). Contrairement à ccid_init(), ce n'est pas un
 * force-link (HID n'a pas de pilote applicatif à tirer dans l'archive) : ce
 * n'est qu'un ré-armement d'état, idempotent, bienvenu à chaque entrée dans
 * le mode.
 */
const uint8_t *mode_otp_fs_config(void)
{
    otp_hid_init();
    return s_fs_config;
}

const uint8_t *mode_otp_hs_config(void)
{
    otp_hid_init();
    return s_hs_config;
}

const char **mode_otp_strings(int *out_count)
{
    s_strings[STRID_SERIAL] = usb_device_serial();
    if (out_count != NULL) {
        *out_count = STRID_COUNT;
    }
    return s_strings;
}
