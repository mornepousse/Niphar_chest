#include "usb/mode_fido.h"

#include <string.h>

#include "tusb.h"

#include "ctaphid.h"
#include "usb/hid_dispatch.h"
#include "usb/usb_device.h"

/*
 * Le mode FIDO encapsule la carte comme authentificateur U2F/CTAP-HID :
 * report HID de 64 octets, en entrée ET en sortie (contrairement au mode
 * OTP, l'hôte nous ÉCRIT — c'est par là qu'arrivent les requêtes CTAP-HID).
 * Ce fichier ne connaît que les descripteurs, les chaînes, et le report
 * descriptor HID, sur le modèle de mode_otp.c — voir usb/mode_fido.h. Le
 * réassemblage des trames reste dans security/ctaphid.c (logique pure,
 * testée sur l'hôte) ; ce fichier ne fait que lui fournir des paquets de 64
 * octets et empaqueter les réponses.
 *
 * Étape 3 du plan FIDO2 (tâche 3) : seules INIT et PING sont câblées. MSG et
 * CBOR rendent CTAPHID_ERR_INVALID_CMD — les tâches suivantes du plan les
 * cableront (U2F d'abord, CBOR ensuite).
 */

enum {
    ITF_NUM_HID = 0,
    ITF_NUM_TOTAL,
};

/* EP0 est réservé ; le HID FIDO prend une OUT et une IN (interrupt) — voir
 * le commentaire en tête de fichier sur le sens des échanges. */
#define EPNUM_FIDO_OUT 0x01
#define EPNUM_FIDO_IN  0x81

#define FIDO_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

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
 * Report descriptor HID — collection application FIDO Alliance (page
 * 0xF1D0, usage 0x01 « authentificateur U2F HID »), 64 octets en entrée ET
 * en sortie. Verbatim du brief de la tâche 3.
 */
static const uint8_t desc_hid_report[] = {
    0x06, 0xD0, 0xF1,        /* USAGE_PAGE (FIDO Alliance)      */
    0x09, 0x01,              /* USAGE (U2F HID Authenticator)   */
    0xA1, 0x01,              /* COLLECTION (Application)        */
    0x09, 0x20,              /*   USAGE (Input Report Data)     */
    0x15, 0x00,              /*   LOGICAL_MINIMUM (0)           */
    0x26, 0xFF, 0x00,        /*   LOGICAL_MAXIMUM (255)         */
    0x75, 0x08,              /*   REPORT_SIZE (8)               */
    0x95, 0x40,              /*   REPORT_COUNT (64)             */
    0x81, 0x02,              /*   INPUT (Data,Var,Abs)          */
    0x09, 0x21,              /*   USAGE (Output Report Data)    */
    0x15, 0x00,              /*   LOGICAL_MINIMUM (0)           */
    0x26, 0xFF, 0x00,        /*   LOGICAL_MAXIMUM (255)         */
    0x75, 0x08,              /*   REPORT_SIZE (8)               */
    0x95, 0x40,              /*   REPORT_COUNT (64)             */
    0x91, 0x02,              /*   OUTPUT (Data,Var,Abs)         */
    0xC0                      /* END_COLLECTION                  */
};

/*
 * Bus-powered, comme les autres modes : le coffre/la carte-clé n'a pas
 * d'autre source que l'USB. Les deux tableaux sont identiques, comme pour
 * mode_otp : CFG_TUD_HID_EP_BUFSIZE (tusb_config.h) ne dépend pas de la
 * vitesse.
 */
static const uint8_t s_fs_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, FIDO_CONFIG_TOTAL_LEN, 0x00, 500),
    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID, STRID_HID, HID_ITF_PROTOCOL_NONE,
                             sizeof(desc_hid_report),
                             EPNUM_FIDO_OUT, EPNUM_FIDO_IN, 64, 5),
};

static const uint8_t s_hs_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, FIDO_CONFIG_TOTAL_LEN, 0x00, 500),
    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID, STRID_HID, HID_ITF_PROTOCOL_NONE,
                             sizeof(desc_hid_report),
                             EPNUM_FIDO_OUT, EPNUM_FIDO_IN, 64, 5),
};

static const uint8_t s_langid_bytes[2] = { 0x09, 0x04 };   /* anglais (US) */

/*
 * STRID_SERIAL vaut NULL ici : le numéro de série dépend de la MAC, connue
 * seulement à l'exécution. mode_fido_strings() le remplit à chaque appel.
 */
static const char *s_strings[STRID_COUNT] = {
    [STRID_LANGID]       = (const char *)s_langid_bytes,
    [STRID_MANUFACTURER] = "Mae PUGIN",
    [STRID_PRODUCT]      = "Cle FIDO2 Niphar",
    [STRID_SERIAL]       = NULL,
    [STRID_HID]          = "Authentificateur U2F",
};

/* ------------------------------------------------------------------------ */
/* Réassemblage CTAP-HID                                                     */
/* ------------------------------------------------------------------------ */

/*
 * Ruling 4 : ctaphid_asm_t pèse 7,6 Kio (buf) à cause de
 * CTAPHID_MAX_PAYLOAD — jamais sur une pile de tâche. Instance statique
 * unique : un seul message CTAP-HID en vol à la fois, ce que
 * CTAPHID_ERR_CHANNEL_BUSY décrit exactement côté protocole (voir
 * ctaphid.c). Remise à zéro à chaque entrée dans le mode (mode_fido_fs_
 * config()/hs_config() ci-dessous), sur le modèle de otp_hid_init() dans
 * mode_otp.c : l'ordre d'évaluation des arguments de usb_device_install()
 * n'étant pas spécifié en C, les deux accesseurs doivent réarmer l'état.
 */
static ctaphid_asm_t s_asm;

/* Dernier CID alloué à un client (0 = aucun) — remis à zéro avec s_asm, pour
 * la même raison. ctaphid_next_cid() ne garde aucun état lui-même (fonction
 * pure), c'est donc à ce fichier de le porter. */
static uint32_t s_last_cid;

/* Version du protocole CTAP-HID : fixée par la spécification, toujours 2. */
#define CTAPHID_PROTOCOL_VERSION 2u

/*
 * Version de PÉRIPHÉRIQUE renvoyée par INIT — PAS NIPHAR_VERSION : cette
 * dernière est une chaîne `git describe` (ex. "v0.3.1-4-gabc1234"), pas un
 * triplet de trois octets numériques. Ce triplet suit la phase du plan
 * FIDO2, pas les tags du firmware ; à faire évoluer quand le protocole
 * CTAP-HID de ce mode se stabilise (MSG et CBOR câblés).
 */
#define FIDO_DEV_VER_MAJOR 0u
#define FIDO_DEV_VER_MINOR 1u
#define FIDO_DEV_VER_BUILD 0u

/*
 * Drapeaux de capacité CTAPHID_CAPFLAG_* du dernier octet d'INIT.
 * CAPFLAG_NMSG (« ce périphérique n'implémente PAS CTAPHID_MSG ») est VRAI à
 * ce stade : MSG rend CTAPHID_ERR_INVALID_CMD (handle_message() plus bas). À
 * retirer quand U2F (MSG) sera câblé — tâche ultérieure du plan. CBOR et
 * WINK restent à 0 pour la même raison : aucun des deux n'est câblé ici.
 */
#define CTAPHID_CAPFLAG_WINK 0x01u
#define CTAPHID_CAPFLAG_CBOR 0x04u
#define CTAPHID_CAPFLAG_NMSG 0x08u

/*
 * Empaquette et envoie une réponse CTAP-HID sur un UNIQUE paquet HID de 64
 * octets (paquet d'initialisation, jamais de continuation).
 *
 * Appelée directement depuis tud_hid_set_report_cb (via handle_message()),
 * donc dans la tâche tud_task : c'est le chemin littéral du brief de la
 * tâche 3, et il tient sur matériel — vérifié par 30 PING consécutifs
 * répondus sans accroc (wt9932_key, 2026-08-17). Une inquiétude de
 * ré-entrance dans hidd_xfer_cb (hid_device.c:416, qui ré-arme l'endpoint
 * OUT juste après cet appel) a été envisagée puis écartée par ce test : la
 * défaillance observée en premier lieu ne venait pas de là, mais d'un banc
 * de test envoyant un rapport de sortie tronqué à 63 octets — un octet de
 * moins que CTAPHID_PKT_SIZE, silencieusement refusé par le garde de
 * fido_set_report() plus bas. Rien à corriger côté firmware.
 *
 * Limitation assumée de cette tâche : contrairement à ctaphid_feed() côté
 * réception (qui réassemble jusqu'à CTAPHID_MAX_PAYLOAD sur plusieurs
 * paquets), l'envoi ne FRAGMENTE PAS. Une charge utile de plus de 57 octets
 * (CTAPHID_PKT_SIZE - 7, la place dans un paquet INIT) est tronquée à 57,
 * avec `len` ajusté sur ce qui est réellement envoyé — jamais un message
 * annoncé plus long que ce qui suit, ce qui ferait attendre l'hôte des
 * paquets CONT qui ne viendront jamais. INIT (17 octets) et un PING usuel
 * tiennent largement dans cette limite. L'envoi fragmenté (nécessaire pour
 * authenticatorGetInfo en CBOR, par exemple) demande tud_hid_report_
 * complete_cb() — un troisième callback HID, hors périmètre de cette tâche
 * limitée à INIT/PING ; à ouvrir avec la tâche CBOR.
 */
static void send_response(uint32_t cid, uint8_t cmd, const uint8_t *data, uint16_t len)
{
    uint8_t pkt[CTAPHID_PKT_SIZE];
    memset(pkt, 0, sizeof(pkt));

    const uint16_t room = (uint16_t)(CTAPHID_PKT_SIZE - 7u);
    const uint16_t n    = (len < room) ? len : room;

    pkt[0] = (uint8_t)(cid >> 24);
    pkt[1] = (uint8_t)(cid >> 16);
    pkt[2] = (uint8_t)(cid >> 8);
    pkt[3] = (uint8_t)cid;
    pkt[4] = (uint8_t)(0x80u | cmd);
    pkt[5] = (uint8_t)(n >> 8);
    pkt[6] = (uint8_t)(n & 0xFFu);
    if (n > 0u && data != NULL) {
        memcpy(pkt + 7, data, n);
    }

    /* Abandon silencieux si l'endpoint n'est pas prêt, jamais d'attente
     * bloquante : ce chemin tourne dans tud_task, l'attendre ici
     * bloquerait la tâche même qui doit libérer l'endpoint. Un seul paquet
     * envoyé par réponse (voir ci-dessus) : l'endpoint est toujours libre
     * à ce point en pratique. */
    if (tud_hid_ready()) {
        tud_hid_report(0, pkt, sizeof(pkt));
    }
}

static void send_error(uint32_t cid, uint8_t err_code)
{
    const uint8_t payload[1] = { err_code };
    send_response(cid, CTAPHID_CMD_ERROR, payload, 1u);
}

/* Traite un message CTAP-HID complet. INIT et PING seulement — voir le
 * commentaire en tête de fichier. */
static void handle_message(const ctaphid_msg_t *msg)
{
    switch (msg->cmd) {
    case CTAPHID_CMD_INIT: {
        /* La charge utile INIT est un nonce de 8 octets, ni plus ni moins —
         * la spécification CTAP-HID n'admet pas d'autre longueur pour cette
         * commande. */
        if (msg->len != 8u) {
            send_error(msg->cid, CTAPHID_ERR_INVALID_LEN);
            return;
        }

        /* Un seul canal client à la fois pour ce périphérique — cohérent
         * avec l'instance unique de s_asm (Ruling 4) : deux canaux réels
         * simultanés supposeraient deux messages en vol. Une requête sur le
         * canal de diffusion en alloue un nouveau ; une requête sur un canal
         * déjà alloué le RÉUTILISE (resynchronisation, cf. ctaphid_feed()
         * qui réarme la transaction dans ce cas). */
        uint32_t new_cid = msg->cid;
        if (msg->cid == CTAPHID_CID_BROADCAST) {
            new_cid    = ctaphid_next_cid(s_last_cid);
            s_last_cid = new_cid;
        }

        uint8_t resp[17];
        memcpy(resp, msg->data, 8u);                 /* nonce échoé */
        resp[8]  = (uint8_t)(new_cid >> 24);
        resp[9]  = (uint8_t)(new_cid >> 16);
        resp[10] = (uint8_t)(new_cid >> 8);
        resp[11] = (uint8_t)new_cid;
        resp[12] = CTAPHID_PROTOCOL_VERSION;
        resp[13] = FIDO_DEV_VER_MAJOR;
        resp[14] = FIDO_DEV_VER_MINOR;
        resp[15] = FIDO_DEV_VER_BUILD;
        resp[16] = CTAPHID_CAPFLAG_NMSG; /* WINK et CBOR : non câblés, à 0 */

        /* Réponse envoyée sur le canal DE LA REQUÊTE (msg->cid), diffusion
         * comprise : le nouveau CID ne voyage que dans la charge utile,
         * jamais dans l'en-tête de ce paquet. */
        send_response(msg->cid, CTAPHID_CMD_INIT, resp, sizeof(resp));
        break;
    }
    case CTAPHID_CMD_PING:
        /* Écho pur : la charge utile revient telle quelle (troncature à 57
         * octets au-delà — voir send_response()). */
        send_response(msg->cid, CTAPHID_CMD_PING, msg->data, msg->len);
        break;
    default:
        /* MSG, CBOR, CANCEL : pas encore câblés — voir le commentaire en
         * tête de fichier. ERR_INVALID_CMD est la réponse prévue par la
         * spécification pour une commande que l'authentificateur ne
         * reconnaît pas. */
        send_error(msg->cid, CTAPHID_ERR_INVALID_CMD);
        break;
    }
}

/* ------------------------------------------------------------------------ */
/* Callbacks TinyUSB de la classe HID                                        */
/* ------------------------------------------------------------------------ */

/*
 * Comme pour mode_otp.c, ces callbacks ne sont jamais des symboles
 * `tud_hid_*_cb` directement : c'est usb/hid_dispatch.c qui les possède et
 * route vers le mode installé. Une seule interface HID à la fois est
 * installée, `instance` vaut donc toujours 0.
 */

static const uint8_t *fido_report_desc(void)
{
    return desc_hid_report;
}

/*
 * TinyUSB livre les données de l'endpoint OUT ici, avec report_id = 0 et
 * report_type = HID_REPORT_TYPE_OUTPUT — confirmé par le commentaire de
 * hid_device.h:137-139 (« received data on OUT endpoint »). Un rapport plus
 * court que CTAPHID_PKT_SIZE n'a aucun octet d'en-tête garanti : ignoré
 * plutôt que passé à ctaphid_feed(), qui lit toujours 64 octets complets.
 */
static void fido_set_report(uint8_t report_id, hid_report_type_t report_type,
                            const uint8_t *buffer, uint16_t bufsize)
{
    (void)report_id;
    if (report_type != HID_REPORT_TYPE_OUTPUT) {
        return;
    }
    if (bufsize < CTAPHID_PKT_SIZE) {
        return;
    }

    ctaphid_msg_t msg;
    const ctaphid_result_t r = ctaphid_feed(&s_asm, buffer, &msg);
    switch (r) {
    case CTAPHID_COMPLETE:
        handle_message(&msg);
        break;
    case CTAPHID_ERROR:
        send_error(msg.err_cid, msg.err_code);
        break;
    case CTAPHID_IN_PROGRESS:
    default:
        break;   /* message incomplet : attendre le prochain paquet CONT */
    }
}

/* Table remise au répartiteur HID unique (usb/hid_dispatch.c) tant que le
 * mode FIDO est actif — voir mode_fido_start()/mode_fido_stop() plus bas.
 * Pas de get_report : ce mode n'échange rien par GET_REPORT/SET_REPORT sur
 * EP0, tout passe par les endpoints OUT/IN dédiés. Le répartiteur rend 0 en
 * son absence, une réponse inerte, jamais un déréférencement nul. */
static const hid_handlers_t s_fido_handlers = {
    .report_desc = fido_report_desc,
    .get_report  = NULL,
    .set_report  = fido_set_report,
};

/* ------------------------------------------------------------------------ */

/*
 * Réarme l'assembleur CTAP-HID (Ruling 4) et le dernier CID alloué à chaque
 * entrée dans le mode — sur le modèle exact de otp_hid_init() dans
 * mode_otp.c : l'ordre d'évaluation des arguments de usb_device_install()
 * n'étant pas spécifié en C, mode_fido_hs_config() peut être appelé avant
 * mode_fido_fs_config(), donc les deux réarment.
 */
const uint8_t *mode_fido_fs_config(void)
{
    ctaphid_reset(&s_asm);
    s_last_cid = 0;
    return s_fs_config;
}

const uint8_t *mode_fido_hs_config(void)
{
    ctaphid_reset(&s_asm);
    s_last_cid = 0;
    return s_hs_config;
}

const char **mode_fido_strings(int *out_count)
{
    s_strings[STRID_SERIAL] = usb_device_serial();
    if (out_count != NULL) {
        *out_count = STRID_COUNT;
    }
    return s_strings;
}

/*
 * Pose la table de handlers FIDO au répartiteur HID unique. À appeler juste
 * après un usb_device_install() réussi vers le mode FIDO — jamais avant :
 * tant que rien n'est installé, router tud_hid_*_cb vers le FIDO n'aurait
 * aucun hôte en face. Symétrique de mode_otp_start().
 */
void mode_fido_start(void)
{
    hid_dispatch_set(&s_fido_handlers);
}

/*
 * Retire la table de handlers FIDO du répartiteur (NULL). À appeler juste
 * AVANT usb_device_uninstall() quand on quitte le mode FIDO — sur le modèle
 * exact de mode_otp_stop() : une fois la désinstallation lancée, plus aucun
 * tud_hid_*_cb ne doit pouvoir retomber sur des handlers dont le mode n'est
 * plus actif.
 */
void mode_fido_stop(void)
{
    hid_dispatch_set(NULL);
}
