#include "usb/mode_pgp.h"

#include "tusb.h"

#include "ccid.h"
#include "ccid_desc.h"
#include "openpgp_card.h"
#include "usb/usb_device.h"

#include "esp_log.h"
#include "esp_mac.h"

/*
 * Le mode PGP encapsule le lecteur de carte à puce CCID (classe 11, USB CCID
 * Rev 1.1) qui porte les APDU OpenPGP jusqu'à openpgp_card.c. Ce fichier ne
 * connaît que les descripteurs et les chaînes, sur le modèle exact de
 * mode_storage.c — voir usb/usb_mode.h. Le protocole CCID lui-même et le
 * worker qui traite les XfrBlock restent dans security/ccid.c, qui ne dépend
 * pas non plus du mode.
 */

static const char *TAG = "mode_pgp";

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
 * Les deux tableaux ne diffèrent QUE par la taille des paquets bulk — 64 en
 * plein débit, 512 en haute vitesse — passée en 5ᵉ paramètre à
 * KASE_CCID_ITF_DESC, sur le modèle exact de TUD_MSC_DESCRIPTOR dans
 * mode_storage.c.
 *
 * Correction tâche 12 : la version d'origine figeait 64 aux deux vitesses
 * (« simplification » supposée sans conséquence pour de petits échanges
 * d'APDU). C'était faux : l'USB 2.0 interdit toute valeur de wMaxPacketSize
 * autre que 512 pour un point bulk en haute vitesse (tableau 5-5 de la
 * spec) ; un descripteur non conforme laissait passer l'énumération mais
 * cassait le pilote CCID interne de scdaemon dès l'ouverture — trouvé en
 * validant `gpg --card-status` sur le coffre, qui négocie la haute vitesse.
 * Voir la divergence documentée en tête de ccid_desc.h.
 */
static const uint8_t s_fs_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, PGP_CONFIG_TOTAL_LEN, 0x00, 500),
    KASE_CCID_ITF_DESC(ITF_NUM_CCID, STRID_CCID, EPNUM_CCID_OUT, EPNUM_CCID_IN, 64),
};

static const uint8_t s_hs_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, PGP_CONFIG_TOTAL_LEN, 0x00, 500),
    KASE_CCID_ITF_DESC(ITF_NUM_CCID, STRID_CCID, EPNUM_CCID_OUT, EPNUM_CCID_IN, 512),
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

/*
 * Symétrique de mode_pgp_data_load() côté sortie de mode. Le fichier reste
 * mince : toute la mécanique est dans ccid_shutdown() (security/ccid.c), ce
 * module ne fait que la relier à usb_mode.c, qui ne connaît que les modes.
 *
 * Ce n'est pas un détail d'ordonnancement : appelée après
 * usb_device_uninstall(), elle arriverait trop tard — la file de tud_task
 * serait déjà détruite, et c'est précisément le créneau que la revue finale a
 * relevé (BLOQUANT 1).
 */
void mode_pgp_stop(void)
{
    ccid_shutdown();
}

/*
 * openpgp_do_init()/openpgp_card_load() ne sont déclarées dans aucun en-tête
 * (même chose en amont KeSp — voir KeSp_firmware/main/main.c) : elles
 * s'appellent par prototype local, comme là-bas.
 */
extern void openpgp_do_init(void);
extern void openpgp_card_load(void);

/*
 * Tâche 12 : câblage du chargement de l'état OpenPGP, absent jusqu'ici (voir
 * task-10-report.md — la carte répondait mais sans DO, PIN ni clé
 * persistés). Deux options se présentaient : au démarrage (main.c, comme
 * sd_probe()/sec_gate_init()) ou à l'entrée du mode PGP.
 *
 * Choix : à l'entrée du mode. Le coffre n'expose et ne charge rien tant
 * qu'aucun mode USB n'est actif (usb_mode.h) — mettre des clés privées en
 * RAM au démarrage, alors qu'aucune interface ne les sert et qu'on ne sait
 * même pas si le mode PGP sera un jour sollicité, contredit cette économie.
 * Le coût est de gérer les entrées/sorties répétées : voir plus bas.
 *
 * Appelée par usb_mode.c, PAS par mode_pgp_fs_config()/hs_config() : ces
 * accesseurs tournent AVANT usb_device_install(), donc avant que TinyUSB
 * n'exécute ccid_drv_init() (le callback .init du pilote CCID, invoqué
 * depuis usbd_init() — synchrone, voir tinyusb/src/device/usbd.c). Or
 * ccid_drv_init() appelle openpgp_card_init(), qui réarme les PIN d'usine EN
 * RAM à chaque bascule vers le mode PGP (pins_factory(), openpgp_card.c).
 * Si openpgp_card_load() tournait avant, ce réarmement écraserait aussitôt
 * après le PIN rechargé depuis la NVS. L'appel doit donc venir APRÈS que
 * usb_device_install() ait réussi — usb_mode.c est le seul endroit qui le
 * sait.
 *
 * Rejouée à chaque entrée dans le mode (et pas seulement la première) parce
 * que openpgp_card_init() réarme les PIN d'usine à CHAQUE bascule, pas
 * seulement au premier boot : sans rechargement systématique, un PIN changé
 * lors d'une session redeviendrait silencieusement le PIN d'usine à la
 * bascule suivante. Toutes les étapes sont idempotentes (relecture NVS,
 * DO déjà présents laissés intacts, numéro de série réécrit à l'identique)
 * donc rejouer la séquence est sans effet de bord.
 */
void mode_pgp_data_load(void)
{
    openpgp_do_init();     /* relit les DO persistés (ou laisse la table vide, carte neuve) */
    openpgp_card_load();   /* relit PIN/retry/clés — doit suivre openpgp_card_init() (ccid.c) */
    if (!openpgp_card_ensure_defaults()) {
        ESP_LOGE(TAG, "amorçage des DO d'usine incomplet");
    }

    uint8_t mac[6] = { 0 };
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        openpgp_card_set_serial(&mac[2]);   /* 4 octets bas de la MAC = numéro de série */
    } else {
        ESP_LOGW(TAG, "lecture MAC échouée — numéro de série AID inchangé");
    }
}
