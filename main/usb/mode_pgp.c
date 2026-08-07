#include "usb/mode_pgp.h"

#include "tusb.h"

#include "ccid.h"
#include "ccid_desc.h"
#include "usb/usb_device.h"

/*
 * Le mode PGP encapsule le lecteur de carte à puce CCID (classe 11, USB CCID
 * Rev 1.1) qui porte les APDU OpenPGP jusqu'à openpgp_card.c. Ce fichier ne
 * connaît que les descripteurs et les chaînes, sur le modèle exact de
 * mode_storage.c — voir usb/usb_mode.h. Le protocole CCID lui-même et le
 * worker qui traite les XfrBlock restent dans security/ccid.c, qui ne dépend
 * pas non plus du mode.
 */

enum {
    ITF_NUM_CCID = 0,
    ITF_NUM_TOTAL,
};

/* EP0 est réservé ; le CCID prend une paire bulk. */
#define EPNUM_CCID_OUT 0x01
#define EPNUM_CCID_IN  0x81

#define PGP_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CCID_DESC_LEN)

/*
 * Les trois premiers index (LANGID/MANUFACTURER/PRODUCT/SERIAL) suivent la
 * convention fixée par usb_device.c — voir USB_STRID_* dans usb_device.c et
 * mode_storage.c. STRID_CCID est propre à ce mode.
 */
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CCID,
    STRID_COUNT,
};

/*
 * Bus-powered, comme mode_storage : le coffre n'a pas d'autre source que
 * l'USB.
 *
 * Les deux tableaux sont identiques : KASE_CCID_ITF_DESC (copié verbatim
 * depuis KeSp — voir ccid_desc.h) fige la taille des paquets bulk à 64
 * octets quelle que soit la vitesse, à la différence de TUD_MSC_DESCRIPTOR
 * qui la reçoit en paramètre. C'est une simplification héritée de l'amont :
 * un échange d'APDU (quelques centaines d'octets) n'a jamais besoin du débit
 * qui justifie les 512 octets du MSC haute vitesse.
 */
static const uint8_t s_fs_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, PGP_CONFIG_TOTAL_LEN, 0x00, 500),
    KASE_CCID_ITF_DESC(ITF_NUM_CCID, STRID_CCID, EPNUM_CCID_OUT, EPNUM_CCID_IN),
};

static const uint8_t s_hs_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, PGP_CONFIG_TOTAL_LEN, 0x00, 500),
    KASE_CCID_ITF_DESC(ITF_NUM_CCID, STRID_CCID, EPNUM_CCID_OUT, EPNUM_CCID_IN),
};

static const uint8_t s_langid_bytes[2] = { 0x09, 0x04 };   /* anglais (US) */

/*
 * STRID_SERIAL vaut NULL ici : le numéro de série dépend de la MAC, connue
 * seulement à l'exécution. mode_pgp_strings() le remplit à chaque appel.
 */
static const char *s_strings[STRID_COUNT] = {
    [STRID_LANGID]       = (const char *)s_langid_bytes,
    [STRID_MANUFACTURER] = "Mae PUGIN",
    [STRID_PRODUCT]      = "Coffre Niphar",
    [STRID_SERIAL]       = NULL,
    [STRID_CCID]         = "Coffre OpenPGP",
};

/*
 * ccid_init() force l'édition de liens de ccid.c.
 *
 * ccid.c définit usbd_app_driver_get_cb() en surcharge FORTE d'un symbole
 * FAIBLE de TinyUSB — c'est le point d'entrée par lequel un pilote de classe
 * applicatif s'enregistre, utilisé ici parce que CCID n'est pas une classe
 * standard de TinyUSB. Tant qu'aucun symbole de ccid.c n'est référencé
 * ailleurs, l'éditeur de liens n'a aucune raison de tirer ccid.o de
 * l'archive (--gc-sections) : le symbole faible par défaut de usbd.c
 * (aucun pilote applicatif) gagnerait alors silencieusement — l'hôte verrait
 * une interface CCID décrite dans les descripteurs, mais sans pilote pour la
 * servir. Cet appel est la référence explicite qui règle ça, reproduite
 * depuis KeSp_firmware/main/comm/usb/usb_hid.c:370 (voir aussi le
 * commentaire de ccid_init() dans ccid.h). Il n'est pas que symbolique :
 * c'est lui qui lance l'auto-test crypto et crée la tâche worker CCID, avant
 * que usb_device_install() ne démarre l'énumération.
 *
 * ccid_init() est idempotent (garde interne), donc l'appeler depuis les deux
 * accesseurs ne fait pas double emploi — et couvre le cas où l'ordre
 * d'évaluation des arguments de usb_device_install() appellerait
 * mode_pgp_hs_config() avant mode_pgp_fs_config() (l'ordre d'évaluation des
 * arguments n'est pas spécifié en C).
 */
const uint8_t *mode_pgp_fs_config(void)
{
    ccid_init();
    return s_fs_config;
}

const uint8_t *mode_pgp_hs_config(void)
{
    ccid_init();
    return s_hs_config;
}

const char **mode_pgp_strings(int *out_count)
{
    s_strings[STRID_SERIAL] = usb_device_serial();
    if (out_count != NULL) {
        *out_count = STRID_COUNT;
    }
    return s_strings;
}
