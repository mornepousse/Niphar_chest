#include "usb/mode_fido.h"

#include <stdbool.h>
#include <string.h>

#include "tusb.h"

#include "ctap2.h"
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
 * retirer quand U2F (MSG) sera câblé — tâche ultérieure du plan. CAPFLAG_CBOR
 * est désormais posé (tâche 5) : authenticatorGetInfo répond sur CBOR — voir
 * le cas CTAPHID_CMD_CBOR ci-dessous. WINK reste à 0 : non câblé.
 *
 * CONTRADICTION INTERNE ASSUMÉE, JUSQU'À LA TÂCHE 7 : authenticatorGetInfo
 * (ctap2.c) annonce "U2F_V2" dans `versions`, alors que ce drapeau dit ici
 * que CTAPHID_MSG — le transport QU'EXIGE U2F — n'est pas implémenté. Ruling
 * du coordinateur (2026-08-18, revue de la tâche 5) : la réponse getInfo dit
 * ce que la clé saura faire À LA FIN DU PLAN, et "U2F_V2" y sera vrai — mais
 * tant que la tâche 7 n'a pas câblé CTAPHID_MSG et U2F, ce drapeau reste VRAI
 * (NMSG posé) et contredit `versions`. RETIRER CTAPHID_CAPFLAG_NMSG d'ici dès
 * que la tâche 7 câble CTAPHID_MSG — pas avant, et pas oublier ensuite : un
 * client qui lit `versions` avant `caps` pourrait tenter U2F_REGISTER contre
 * un périphérique qui l'annonce indisponible.
 */
#define CTAPHID_CAPFLAG_WINK 0x01u
#define CTAPHID_CAPFLAG_CBOR 0x04u
#define CTAPHID_CAPFLAG_NMSG 0x08u

/*
 * État d'émission fragmentée — image miroir de ctaphid_asm_t côté envoi :
 * même argument (Ruling 4) qu'un seul message en vol à la fois, donc une
 * seule réponse en cours d'émission à tout instant. `buf` reprend la taille
 * maximale d'un message CTAP-HID (CTAPHID_MAX_PAYLOAD, 7,6 Kio) — jamais sur
 * une pile de tâche, pour la même raison que s_asm.
 *
 * Nécessaire dès qu'une réponse dépasse 57 octets (la place dans un paquet
 * INIT) : authenticatorGetInfo (CBOR, tâche 5) porte les versions, un AAGUID
 * de 16 octets, les options et maxMsgSize — bien au-delà. Ruling du
 * coordinateur (2026-08-17) : la fragmentation est du code de TRANSPORT,
 * elle appartient ici, pas à la tâche CBOR qui viendra ensuite.
 */
typedef struct {
    uint8_t  buf[CTAPHID_MAX_PAYLOAD];
    uint16_t len;         /* longueur totale du message à émettre */
    uint16_t sent;        /* octets déjà copiés dans un paquet déjà envoyé */
    uint32_t cid;
    uint8_t  cmd;
    uint8_t  next_seq;    /* prochain numéro de séquence CONT, 0..127 */
} fido_tx_t;

static fido_tx_t s_tx;

/*
 * Vrai tant qu'il reste au moins un paquet de continuation à émettre pour la
 * réponse en cours (voir send_response()/fido_report_complete()). Toute
 * NOUVELLE requête reçue pendant ce temps est refusée avec
 * CTAPHID_ERR_CHANNEL_BUSY, sur SON PROPRE canal — jamais celui de
 * l'émission en cours — plutôt que de laisser send_response() écraser s_tx
 * en plein vol. Ruling du coordinateur (2026-08-17) : « un hôte bien élevé
 * attend, un hôte hostile n'attend pas » — sur une clé de sécurité, une
 * réponse tronquée ou mélangée entre deux canaux est une confusion de
 * contexte dans le transport qui portera bientôt des signatures.
 * CTAPHID_ERR_CHANNEL_BUSY existe exactement pour cet usage dans la
 * spécification ; rien n'est inventé ici.
 *
 * Libéré sur TOUS les chemins de sortie de l'émission — y compris un échec
 * de tud_hid_report(), premier paquet ou continuation — jamais seulement au
 * dernier paquet réussi : un drapeau occupé qui resterait coincé
 * transformerait un défaut d'endpoint passager en clé définitivement
 * muette, jusqu'au débranchement.
 */
static bool s_tx_busy;

/*
 * CID en attente de refus CTAPHID_ERR_CHANNEL_BUSY : posé quand send_
 * busy_refusal() ne peut pas partir tout de suite (endpoint occupé — voir
 * plus bas), consommé par fido_report_complete() dès que l'endpoint se
 * libère. Un seul CID mémorisé : une deuxième collision pendant que la
 * première attend déjà son tour écrase le premier CID, celui-ci n'obtenant
 * alors aucun refus explicite — l'hôte le découvre par timeout, comme tout
 * message auquel ce périphérique ne répond jamais. Suffisant pour l'usage
 * visé (une clé de sécurité ne parle qu'à un client à la fois) et cohérent
 * avec Ruling 4 (un seul message CTAP-HID vraiment « en cours » ici).
 */
static bool     s_refusal_pending;
static uint32_t s_refusal_cid;

/* Construit un paquet ERROR isolé (jamais suivi de continuation) pour
 * `cid`/`err_code`, et tente de l'envoyer immédiatement. Rend vrai si le
 * paquet est réellement parti. */
static bool try_send_error_now(uint32_t cid, uint8_t err_code)
{
    uint8_t pkt[CTAPHID_PKT_SIZE];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = (uint8_t)(cid >> 24);
    pkt[1] = (uint8_t)(cid >> 16);
    pkt[2] = (uint8_t)(cid >> 8);
    pkt[3] = (uint8_t)cid;
    pkt[4] = (uint8_t)(0x80u | CTAPHID_CMD_ERROR);
    pkt[5] = 0;
    pkt[6] = 1;
    pkt[7] = err_code;
    return tud_hid_ready() && tud_hid_report(0, pkt, sizeof(pkt));
}

/*
 * Refuse une nouvelle requête reçue pendant qu'une réponse précédente est
 * encore en vol, SANS toucher à s_tx : passer par send_response() pour ce
 * refus écraserait s_tx.buf/len/cid en plein envoi, corrompant les paquets
 * de continuation qui restent à partir pour CETTE réponse.
 *
 * L'endpoint IN est très souvent occupé PAR CETTE MÊME réponse au moment où
 * le refus voudrait partir (constaté sur matériel, wt9932_key, 2026-08-17 :
 * une collision au milieu d'une réponse de 300 octets tombait quasi
 * systématiquement dans cette fenêtre). Un simple abandon silencieux dans
 * ce cas perdrait le refus la plupart du temps, pas seulement dans un cas
 * limite — inacceptable pour ce que ce mécanisme doit prouver. Le refus est
 * donc mis en ATTENTE (s_refusal_pending/s_refusal_cid) si l'envoi immédiat
 * échoue ; fido_report_complete() le prend en charge en priorité dès que
 * l'endpoint se libère, avant de reprendre la continuation de s_tx.
 */
static void send_busy_refusal(uint32_t cid)
{
    if (try_send_error_now(cid, CTAPHID_ERR_CHANNEL_BUSY)) {
        return;
    }
    s_refusal_pending = true;
    s_refusal_cid     = cid;
}

/*
 * Empaquette et envoie une réponse CTAP-HID, fragmentée si nécessaire sur
 * plusieurs paquets — l'image miroir exacte de ce que ctaphid_feed() sait
 * défaire côté réception : un paquet d'initialisation (CID, CMD|0x80, BCNT
 * sur 16 bits, jusqu'à 57 octets de charge), puis des paquets de
 * continuation (CID, numéro de séquence 0..127, jusqu'à 59 octets).
 *
 * Un message de plus de CTAPHID_MAX_PAYLOAD (57 + 128×59 = 7609) est
 * IMPOSSIBLE à représenter en CTAP-HID — le numéro de séquence CONT ne tient
 * que sur 7 bits, 128 paquets de continuation au plus. Refusé explicitement
 * ici (rien n'est envoyé) plutôt que tronqué en silence : un appelant qui
 * produit une réponse plus grande a un bug, mieux vaut un message absent
 * qu'un message mal formé sur le fil, que l'hôte prendrait pour argent
 * comptant. Cette garde n'est atteignable par aucune requête externe
 * aujourd'hui (ctaphid_feed() borne déjà tout message REÇU à 7609 octets),
 * c'est une défense contre un futur appelant fautif (CBOR) — coût nul,
 * gardée telle quelle.
 *
 * Le premier paquet part directement d'ici, dans tud_hid_set_report_cb (via
 * handle_message()) — chemin vérifié sur matériel par 30 PING consécutifs
 * sans accroc (wt9932_key, 2026-08-17), y compris sans le mécanisme de
 * continuation ci-dessous. Les paquets de continuation, eux, NE PEUVENT PAS
 * partir du même appel synchrone : la classe HID de TinyUSB n'admet qu'un
 * seul transfert IN en vol à la fois (tud_hid_ready() reste faux tant que
 * l'hôte n'a pas consommé le précédent), et ce chemin tourne déjà dans
 * tud_task — l'attendre ici bloquerait la tâche même qui doit libérer
 * l'endpoint. fido_report_complete() ci-dessous (chaîné via le répartiteur
 * HID, tud_hid_report_complete_cb) prend donc le relais paquet par paquet,
 * exactement le patron que hid_device.c attend de tud_hid_report_
 * complete_cb — c'est là, et seulement là, qu'un nouvel appel à
 * tud_hid_report() est sûr après le premier.
 */
static void send_response(uint32_t cid, uint8_t cmd, const uint8_t *data, uint16_t len)
{
    if (s_tx_busy) {
        /* Une réponse précédente est encore en cours d'émission : refuser
         * ce nouveau message plutôt que d'écraser s_tx — voir s_tx_busy. */
        send_busy_refusal(cid);
        return;
    }

    if (len > CTAPHID_MAX_PAYLOAD) {
        return;   /* refus explicite — voir le commentaire ci-dessus */
    }

    /* Copié maintenant, jamais lu plus tard directement depuis `data` : elle
     * pointe souvent dans s_asm.buf (cas PING), que le prochain paquet
     * OUT — accepté dès que ce paquet-ci sera parti — peut réécrire avant
     * que toutes les continuations n'aient été émises. */
    if (len > 0u && data != NULL) {
        memcpy(s_tx.buf, data, len);
    }
    s_tx.len      = len;
    s_tx.cid      = cid;
    s_tx.cmd      = cmd;
    s_tx.next_seq = 0;   /* la séquence CONT repart de 0 à CHAQUE message */

    uint8_t pkt[CTAPHID_PKT_SIZE];
    memset(pkt, 0, sizeof(pkt));

    const uint16_t room = (uint16_t)(CTAPHID_PKT_SIZE - 7u);   /* 57 */
    const uint16_t n    = (len < room) ? len : room;

    pkt[0] = (uint8_t)(cid >> 24);
    pkt[1] = (uint8_t)(cid >> 16);
    pkt[2] = (uint8_t)(cid >> 8);
    pkt[3] = (uint8_t)cid;
    pkt[4] = (uint8_t)(0x80u | cmd);
    pkt[5] = (uint8_t)(len >> 8);   /* longueur TOTALE, jamais tronquée */
    pkt[6] = (uint8_t)(len & 0xFFu);
    if (n > 0u) {
        memcpy(pkt + 7, s_tx.buf, n);
    }
    s_tx.sent = n;

    /* Rien n'est en vol avant ce premier paquet (Ruling 4 + s_tx_busy tout
     * juste vérifié faux), donc l'endpoint est attendu libre. Un échec ici
     * (non prêt, ou tud_hid_report() refuse la mise en file) abandonne
     * l'émission au lieu de poser s_tx_busy : jamais d'occupation posée sur
     * un paquet qui n'est pas réellement parti — voir s_tx_busy. */
    if (!tud_hid_ready() || !tud_hid_report(0, pkt, sizeof(pkt))) {
        return;
    }

    s_tx_busy = (s_tx.sent < s_tx.len);
}

/*
 * Suite de send_response() : émet le prochain paquet de continuation, s'il
 * en reste — appelée par le répartiteur HID (usb/hid_dispatch.c) à chaque
 * fois qu'un rapport IN part effectivement sur le fil (tud_hid_report_
 * complete_cb).
 *
 * PRIORITÉ absolue à un refus CTAPHID_ERR_CHANNEL_BUSY resté en attente
 * (voir send_busy_refusal()) : l'endpoint vient tout juste de se libérer,
 * c'est le moment le plus fiable pour le faire enfin partir. La continuation
 * de s_tx, elle, attendra le PROCHAIN report_complete — celui que ce paquet
 * de refus va lui-même déclencher une fois parti. Aucune donnée de s_tx
 * n'est touchée par ce détour : la réponse en cours reprend exactement là
 * où elle en était.
 *
 * s_tx_busy faux (et rien en attente) signifie qu'aucune suite n'est
 * attendue pour s_tx (réponse tenue dans l'unique paquet d'initialisation,
 * cas le plus fréquent — INIT, PING usuel) : c'est le report_complete du
 * DERNIER paquet d'une telle réponse qui arrive ici, comportement normal et
 * silencieux.
 */
static void fido_report_complete(const uint8_t *report, uint16_t len)
{
    (void)report;
    (void)len;

    if (s_refusal_pending) {
        s_refusal_pending = false;
        /* Échec possible ici aussi (improbable juste après une libération,
         * mais pas impossible) : abandonné sans réessai — un deuxième essai
         * bloquerait la reprise de s_tx qui suit. */
        try_send_error_now(s_refusal_cid, CTAPHID_ERR_CHANNEL_BUSY);
        return;
    }

    if (!s_tx_busy) {
        return;
    }
    if (s_tx.sent >= s_tx.len) {
        s_tx_busy = false;   /* ne devrait pas arriver si s_tx_busy est correct, mais ferme la boucle */
        return;
    }

    uint8_t pkt[CTAPHID_PKT_SIZE];
    memset(pkt, 0, sizeof(pkt));

    pkt[0] = (uint8_t)(s_tx.cid >> 24);
    pkt[1] = (uint8_t)(s_tx.cid >> 16);
    pkt[2] = (uint8_t)(s_tx.cid >> 8);
    pkt[3] = (uint8_t)s_tx.cid;
    pkt[4] = (uint8_t)(s_tx.next_seq & 0x7Fu);   /* bit 7 bas : continuation */

    const uint16_t room      = (uint16_t)(CTAPHID_PKT_SIZE - 5u);   /* 59 */
    const uint16_t remaining = (uint16_t)(s_tx.len - s_tx.sent);
    const uint16_t n         = (remaining < room) ? remaining : room;
    memcpy(pkt + 5, s_tx.buf + s_tx.sent, n);
    s_tx.sent = (uint16_t)(s_tx.sent + n);
    s_tx.next_seq++;

    /* Le transfert précédent vient juste de se terminer (c'est ce qui
     * déclenche cet appel) : l'endpoint est donc attendu prêt. Un échec ici
     * abandonne l'émission (libère s_tx_busy) plutôt que de rester coincé —
     * l'hôte verra un message incomplet et devra réessayer, ce que CTAP-HID
     * gère déjà par timeout côté hôte. */
    if (!tud_hid_ready() || !tud_hid_report(0, pkt, sizeof(pkt))) {
        s_tx_busy = false;
        return;
    }

    if (s_tx.sent >= s_tx.len) {
        s_tx_busy = false;   /* dernier paquet parti */
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
        resp[16] = CTAPHID_CAPFLAG_NMSG | CTAPHID_CAPFLAG_CBOR; /* WINK : non câblé, à 0 */

        /* Réponse envoyée sur le canal DE LA REQUÊTE (msg->cid), diffusion
         * comprise : le nouveau CID ne voyage que dans la charge utile,
         * jamais dans l'en-tête de ce paquet. */
        send_response(msg->cid, CTAPHID_CMD_INIT, resp, sizeof(resp));
        break;
    }
    case CTAPHID_CMD_PING:
        /* Écho pur : la charge utile revient telle quelle, fragmentée si
         * besoin par send_response() — msg->len peut dépasser 57 octets,
         * ctaphid_feed() l'a déjà réassemblée le cas échéant. */
        send_response(msg->cid, CTAPHID_CMD_PING, msg->data, msg->len);
        break;
    case CTAPHID_CMD_CBOR: {
        /*
         * Une réponse CTAP2 (succès ou échec) voyage TOUJOURS dans une
         * trame CTAPHID_CMD_CBOR, jamais dans une trame CTAPHID_CMD_ERROR :
         * cette dernière est réservée aux erreurs de TRANSPORT (canal,
         * séquence — voir ctaphid.h), pas aux erreurs de la commande CTAP2
         * elle-même. C'est pourquoi les branches ci-dessous appellent
         * send_response(..., CTAPHID_CMD_CBOR, ...) même en cas d'échec, un
         * simple octet de statut CTAP2_ERR_* servant alors de charge utile.
         */
        if (msg->len < 1u) {
            /* Aucun octet de commande à interpréter. */
            const uint8_t status = CTAP2_ERR_INVALID_LENGTH;
            send_response(msg->cid, CTAPHID_CMD_CBOR, &status, 1u);
            break;
        }
        if (msg->data[0] != CTAP2_CMD_GET_INFO) {
            /* Seul authenticatorGetInfo est câblé à ce stade (tâche 5) —
             * makeCredential/getAssertion suivront avec le décodeur CBOR,
             * reporté au plan 2 (aucun paramètre de requête ici : voir
             * contraintes-globales.md, écart assumé 2). */
            const uint8_t status = CTAP2_ERR_INVALID_COMMAND;
            send_response(msg->cid, CTAPHID_CMD_CBOR, &status, 1u);
            break;
        }

        /* 128 octets : la réponse authenticatorGetInfo tient en 39 (statut
         * compris — voir ctap2.c), large marge sans dépasser ce que
         * send_response()/CTAPHID_MAX_PAYLOAD sauraient de toute façon
         * gérer si ce champ grossissait plus tard. */
        uint8_t resp[128];
        size_t  n = ctap2_build_get_info(resp, sizeof resp);
        if (n == 0u) {
            /* Ne devrait jamais arriver (tampon largement suffisant) —
             * défense plutôt que silence : mieux vaut un message d'erreur
             * explicite qu'une trame absente que l'hôte prendrait pour un
             * périphérique muet. CTAP2_ERR_OTHER, pas INVALID_LENGTH : cet
             * échec porte sur la construction de la RÉPONSE (place dans un
             * tampon interne), rien à voir avec la longueur de la requête
             * reçue — la branche juste au-dessus couvre déjà ce cas-là. */
            const uint8_t status = CTAP2_ERR_OTHER;
            send_response(msg->cid, CTAPHID_CMD_CBOR, &status, 1u);
            break;
        }
        send_response(msg->cid, CTAPHID_CMD_CBOR, resp, (uint16_t)n);
        break;
    }
    default:
        /* MSG, CANCEL : pas encore câblés — voir le commentaire en tête de
         * fichier. ERR_INVALID_CMD est la réponse prévue par la
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
 * EP0, tout passe par les endpoints OUT/IN dédiés. report_complete chaîne
 * les paquets de continuation d'une réponse fragmentée — voir send_response()
 * et fido_report_complete(). Le répartiteur rend 0/aucune action en
 * l'absence d'un champ, jamais un déréférencement nul. */
static const hid_handlers_t s_fido_handlers = {
    .report_desc     = fido_report_desc,
    .get_report      = NULL,
    .set_report      = fido_set_report,
    .report_complete = fido_report_complete,
};

/* ------------------------------------------------------------------------ */

/*
 * Réarme l'assembleur CTAP-HID (Ruling 4), le dernier CID alloué et l'état
 * d'émission (y compris s_tx_busy) à chaque entrée dans le mode — sur le
 * modèle exact de otp_hid_init() dans mode_otp.c : l'ordre d'évaluation des
 * arguments de usb_device_install() n'étant pas spécifié en C,
 * mode_fido_hs_config() peut être appelé avant mode_fido_fs_config(), donc
 * les deux réarment.
 */
const uint8_t *mode_fido_fs_config(void)
{
    ctaphid_reset(&s_asm);
    s_last_cid   = 0;
    s_tx.len     = 0;
    s_tx.sent    = 0;
    s_tx_busy    = false;
    s_refusal_pending = false;
    return s_fs_config;
}

const uint8_t *mode_fido_hs_config(void)
{
    ctaphid_reset(&s_asm);
    s_last_cid   = 0;
    s_tx.len     = 0;
    s_tx.sent    = 0;
    s_tx_busy    = false;
    s_refusal_pending = false;
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
