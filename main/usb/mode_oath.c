#include "usb/mode_oath.h"

#include <string.h>

#include "tusb.h"

#include "apdu.h"
#include "ccid.h"
#include "ccid_desc.h"
#include "cr_hmac.h"
#include "oath_name.h"
#include "oath_proto.h"
#include "sec_confirm.h"
#include "sec_store.h"
#include "usb/usb_device.h"

#include "esp_log.h"
#include "esp_random.h"

#include "nvs.h"         /* ESP_ERR_NVS_NOT_FOUND */
#include "nvs_utils.h"
#include "sec_config.h"  /* STORAGE_NAMESPACE */

/*
 * Le mode OATH encapsule l'applet YKOATH (comptes TOTP) sur le MÊME transport
 * CCID que le mode PGP. Ce fichier porte trois choses, et rien d'autre :
 *
 *   1. les descripteurs et les chaînes (comme mode_pgp.c) ;
 *   2. le sel du SELECT, tiré une fois et persisté ;
 *   3. le seul endroit du firmware où une confirmation physique se traduit en
 *      destruction de secrets — voir oath_apdu() plus bas.
 *
 * Le protocole lui-même est dans security/oath_proto.c, pur et testé sur
 * l'hôte. Le partage est délibéré : oath_proto ne touche à aucun
 * périphérique et ne bloque jamais, donc il ne peut pas attendre un appui —
 * il se contente de DIRE qu'il en faut un (OATH_SW_NEEDS_TOUCH).
 *
 * ------------------------------------------------------------------------
 * CE QUE CE FICHIER GARANTIT, ET QU'AUCUN TEST HÔTE NE PEUT VÉRIFIER
 * ------------------------------------------------------------------------
 *
 * oath_touch_commit() CROIT son paramètre `granted` : elle efface des comptes
 * sur la foi d'un booléen, sans vérifier quoi que ce soit (voir l'avertissement
 * en tête de sa déclaration, security/oath_proto.h). La propriété « aucun
 * secret ne part sans un geste physique » ne tient donc que par les deux
 * appels de ce fichier, et par eux seuls. Elle est intenable par un test :
 * l'appui est du matériel.
 *
 * Deux règles, à relire avant de toucher à oath_apdu() :
 *
 *   - `granted` ne doit JAMAIS valoir autre chose que le retour de
 *     ccid_confirm_named(). Pas un `true` littéral, pas une variable dont un
 *     chemin d'erreur pourrait sortir vraie, pas un « on saute la confirmation
 *     si … ». Un `oath_touch_commit(&s_ctx, true, …)` posé ici vide le magasin
 *     sans qu'on ait rien demandé à personne.
 *
 *   - l'attente doit rester BLOQUANTE. `touch_slot` est un INDEX, pas une
 *     identité : la revérification faite dans oath_touch_commit() contrôle que
 *     c'est toujours un slot OATH, pas que c'est le même compte. Ce qui rend
 *     l'index suffisant, c'est que ccid.c bloque le worker — donc l'hôte —
 *     entre la demande et l'appui, si bien qu'aucune autre commande ne peut
 *     s'intercaler. Rendre cette attente non bloquante casserait la garantie
 *     sans qu'aucun test ne le dise.
 *
 * ------------------------------------------------------------------------
 * DÉCOUVERTE DE L'APPAREIL PAR ykman — À TRANCHER EN VALIDATION (tâche 7)
 * ------------------------------------------------------------------------
 *
 * Les chaînes ci-dessous sont HONNÊTES : le coffre ne se déclare pas YubiKey.
 * Conséquence à connaître avant de brancher : `ykman` ne parle pas à un
 * lecteur PC/SC quelconque — il filtre les lecteurs dont le NOM contient
 * « yubico yubikey » (YK_READER_NAME, ykman/pcsc/__init__.py). Un coffre
 * nommé « Coffre Niphar » n'apparaîtra donc pas dans `ykman list`, et il
 * faudra passer `ykman --reader <nom>` — l'option existe précisément pour ça.
 *
 * S'y ajoute, en amont, la question de savoir si pcscd/libccid réclame un
 * appareil de VID/PID 303A:4021 (usb/usb_device.c), qui ne figure dans aucune
 * liste amont. Le mode PGP, lui, ne rencontre pas ce problème : scdaemon a son
 * propre pilote CCID interne et se passe de PC/SC.
 *
 * Rien de tout cela ne se règle dans le firmware sans mentir sur l'identité de
 * l'appareil, ce que la spec refuse explicitement (« C'est une annonce de
 * compatibilité protocolaire, pas une prétention d'être un YubiKey »,
 * docs/superpowers/specs/2026-08-18-oath-totp-design.md). C'est une question
 * de configuration hôte, à trancher avec Mae en tâche 7.
 */

static const char *TAG = "mode_oath";

enum {
    ITF_NUM_CCID = 0,
    ITF_NUM_TOTAL,
};

/* EP0 est réservé ; le CCID prend une paire bulk. Mêmes adresses que le mode
 * PGP : les deux modes ne coexistent jamais (usb_mode.c). */
#define EPNUM_CCID_OUT 0x01
#define EPNUM_CCID_IN  0x81

#define OATH_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CCID_DESC_LEN)

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CCID,
    STRID_COUNT,
};

/*
 * Bus-powered, comme mode_pgp. Les deux tableaux ne diffèrent QUE par la
 * taille des paquets bulk — 64 en plein débit, 512 en haute vitesse. Ce n'est
 * pas une simplification qu'on pourrait aplatir : l'USB 2.0 interdit toute
 * autre valeur que 512 pour un point bulk en haute vitesse (tableau 5-5), et
 * figer 64 aux deux vitesses a déjà cassé le mode PGP sur matériel — voir la
 * correction tâche 12 en tête de mode_pgp.c.
 */
static const uint8_t s_fs_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, OATH_CONFIG_TOTAL_LEN, 0x00, 500),
    KASE_CCID_ITF_DESC(ITF_NUM_CCID, STRID_CCID, EPNUM_CCID_OUT, EPNUM_CCID_IN, 64),
};

static const uint8_t s_hs_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, OATH_CONFIG_TOTAL_LEN, 0x00, 500),
    KASE_CCID_ITF_DESC(ITF_NUM_CCID, STRID_CCID, EPNUM_CCID_OUT, EPNUM_CCID_IN, 512),
};

static const uint8_t s_langid_bytes[2] = { 0x09, 0x04 };   /* anglais (US) */

/* STRID_SERIAL vaut NULL ici : le numéro de série dépend de la MAC, connue
 * seulement à l'exécution. mode_oath_strings() le remplit à chaque appel. */
static const char *s_strings[STRID_COUNT] = {
    [STRID_LANGID]       = (const char *)s_langid_bytes,
    [STRID_MANUFACTURER] = "Mae PUGIN",
    [STRID_PRODUCT]      = "Coffre Niphar",
    [STRID_SERIAL]       = NULL,
    /* Distinct de « Coffre OpenPGP » : c'est la chaîne d'interface CCID, celle
     * qui identifie l'applet côté hôte. Deux modes qui partagent le transport
     * doivent au moins se nommer différemment dans les traces. */
    [STRID_CCID]         = "Coffre OATH",
};

/*
 * L'état de l'applet. Une seule instance : le worker CCID est unique et
 * sérialise les XfrBlock (bMaxCCIDBusySlots=1), donc il n'y a jamais deux
 * commandes en vol.
 *
 * Porte un secret en clair pendant qu'un PUT qui écrase attend sa
 * confirmation (`touch_put_secret`) : mode_oath_stop() l'efface en entier à
 * la sortie du mode.
 */
static oath_ctx_t s_ctx;

/* ------------------------------------------------------------------ */
/* Le sel du SELECT                                                    */
/* ------------------------------------------------------------------ */

/*
 * ykman en fait un SHA-256 pour nommer l'appareil (_get_device_id, oath.py).
 * Il doit donc être STABLE d'une session à l'autre : un sel retiré à chaque
 * branchement ferait voir un appareil différent à chaque fois, et ykman
 * refuserait les identifiants enregistrés.
 *
 * Tiré au sort et non dérivé de la MAC : l'identifiant publié à l'hôte n'a pas
 * à révéler un identifiant matériel. Il n'est pas secret pour autant — il
 * sort en clair dans la réponse au SELECT — mais il n'a aucune raison d'être
 * un lien vers autre chose.
 */
#define OATH_SALT_NVS_KEY "oath_salt"
#define OATH_SALT_NVS_VER "oath_salt_ver"

static void oath_salt_load(uint8_t out[OATH_SALT_LEN])
{
    uint32_t ver = 0;
    const esp_err_t err = nvs_load_blob_with_total(STORAGE_NAMESPACE, OATH_SALT_NVS_KEY,
                                                   out, OATH_SALT_LEN,
                                                   OATH_SALT_NVS_VER, &ver);
    if (err == ESP_OK && ver == 1u) {
        return;
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        /* Journalisé et non fatal : on repart sur un sel neuf. L'hôte verra un
         * appareil différent — visible, contrariant, mais honnête. Se taire
         * ferait passer une NVS en panne pour une première utilisation. */
        ESP_LOGW(TAG, "relecture du sel : %s — un nouveau sel est tiré",
                 esp_err_to_name(err));
    }

    esp_fill_random(out, OATH_SALT_LEN);
    if (nvs_save_blob_with_total(STORAGE_NAMESPACE, OATH_SALT_NVS_KEY,
                                 out, OATH_SALT_LEN, OATH_SALT_NVS_VER, 1u) != ESP_OK) {
        /* Le mode reste utilisable pour cette session ; c'est la SUIVANTE qui
         * verra un autre sel. Le dire ici est la seule chance de faire le lien
         * entre « ykman ne reconnaît plus la clé » et une NVS qui n'écrit
         * plus. */
        ESP_LOGE(TAG, "sel non persisté — ykman verra un autre appareil au prochain branchement");
    }
}

/* ------------------------------------------------------------------ */
/* Réponses                                                            */
/* ------------------------------------------------------------------ */

/*
 * Mots d'état écrits par CE fichier. Ils doublent ceux de oath_proto.c, qui
 * les garde privés — un doublon de deux constantes ISO vaut mieux que
 * d'ouvrir un en-tête sur des noms aussi génériques que SW_OK, déjà définis
 * ailleurs (security/openpgp_card.c en a sa propre table).
 */
#define OATH_SW_OK           0x9000u
#define OATH_SW_COND_NOT_SAT 0x6985u   /* appui refusé ou expiré */
#define OATH_SW_NOT_FOUND    0x6A82u
#define OATH_SW_FULL         0x6A84u
#define OATH_SW_MEMORY       0x6581u
#define OATH_SW_WRONG_LENGTH 0x6700u

static uint16_t oath_sw(uint8_t *out, uint16_t cap, uint16_t sw)
{
    if (cap < 2) return 0;
    out[0] = (uint8_t)(sw >> 8);
    out[1] = (uint8_t)(sw & 0xFFu);
    return 2;
}

/* ------------------------------------------------------------------ */
/* Achèvement d'un CALCULATE                                           */
/* ------------------------------------------------------------------ */

/*
 * CALCULATE ne passe PAS par oath_touch_commit() : produire le code demande le
 * HMAC, qui n'est pas de la logique pure (oath_proto.h le dit et rend 0 pour
 * ce cas). C'est donc ici, et la demande en attente se consomme ici aussi.
 *
 * Format de la réponse, relevé dans yubikit/oath.py de ykman 5.9.1 :
 *   - P2=01 (calculate_code) : 0x76, valeur = [chiffres][4 octets] ;
 *     _format_code() fait (bytes2int(valeur[1:]) & 0x7FFFFFFF) % 10**chiffres.
 *   - P2=00 (calculate)      : 0x75, valeur = [chiffres][HMAC complet].
 * Le modulo est fait par l'hôte — oath_dynamic_binary() ne le fait pas, et le
 * faire ici rendrait des codes faux (voir son commentaire).
 */
static uint16_t oath_finish_calculate(bool granted, uint8_t *out, uint16_t cap)
{
    if (cap < 2) return 0;

    /* Tout ce dont on a besoin est copié AVANT de consommer la demande. */
    const uint8_t slot     = s_ctx.touch_slot;
    const bool    tronque  = s_ctx.touch_truncate;
    uint8_t challenge[OATH_CHALLENGE_LEN];
    memcpy(challenge, s_ctx.touch_challenge, sizeof(challenge));

    /*
     * La demande se consomme sur TOUS les chemins de sortie, y compris les
     * erreurs : une confirmation ne doit pas pouvoir servir une seconde fois.
     * oath_dispatch() purge aussi à chaque commande suivante — mais dépendre
     * de cette purge distante ferait tenir la propriété par un nettoyage
     * qu'on retire un jour sans voir le rapport.
     */
    s_ctx.touch_op    = OATH_TOUCH_NONE;
    s_ctx.touch_count = 0;

    if (!granted) {
        return oath_sw(out, cap, OATH_SW_COND_NOT_SAT);
    }

    /*
     * Le slot est revérifié APRÈS l'appui, comme le fait oath_touch_commit()
     * pour les trois autres opérations : `touch_slot` est un index, et ce qu'il
     * désigne doit toujours appartenir à cet applet. Rien ne peut avoir changé
     * tant que l'attente est bloquante — c'est une défense en profondeur, pas
     * la garantie elle-même (voir l'en-tête de ce fichier).
     */
    if (slot >= SEC_N_SLOTS || !oath_slot_is_oath(slot)) {
        return oath_sw(out, cap, OATH_SW_NOT_FOUND);
    }

    uint8_t secret[SEC_SECRET_MAX];
    uint8_t secret_len = 0;
    if (!sec_store_get_secret(slot, secret, &secret_len)) {
        memset(secret, 0, sizeof(secret));
        return oath_sw(out, cap, OATH_SW_NOT_FOUND);
    }

    uint8_t hmac[20];
    const bool ok = cr_hmac_sha1(secret, secret_len,
                                 challenge, (uint16_t)sizeof(challenge), hmac);
    /* Le secret ne survit pas à cette pile : il n'a rien à faire en mémoire
     * une fois le HMAC calculé. */
    memset(secret, 0, sizeof(secret));
    if (!ok) {
        memset(hmac, 0, sizeof(hmac));
        return oath_sw(out, cap, OATH_SW_MEMORY);
    }

    /*
     * Le nombre de chiffres vient du slot, jamais d'un défaut posé ici :
     * l'hôte s'en sert TEL QUEL pour son modulo, donc une valeur inventée
     * rendrait un code de la mauvaise longueur, plausible et faux. Un slot
     * OATH sans chiffres est un état interne incohérent — oath_do_put() ne
     * laisse passer que 6 ou 8 — et il se refuse plutôt que de se deviner.
     */
    const uint8_t digits = sec_store_digits(slot);
    if (digits != 6u && digits != 8u) {
        memset(hmac, 0, sizeof(hmac));
        ESP_LOGE(TAG, "slot %u sans nombre de chiffres — code refusé", (unsigned)slot);
        return oath_sw(out, cap, OATH_SW_MEMORY);
    }

    /* Deux octets réservés au mot d'état : le TLV s'écrit dans ce qui reste. */
    const uint16_t tlv_cap = (uint16_t)(cap - 2u);
    uint16_t n;
    if (tronque) {
        const uint32_t v = oath_dynamic_binary(hmac, (uint8_t)sizeof(hmac));
        const uint8_t val[1 + 4] = {
            digits,
            (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v,
        };
        n = oath_tlv_put(out, tlv_cap, OATH_TAG_TRUNCATED, val, (uint16_t)sizeof(val));
    } else {
        uint8_t val[1 + sizeof(hmac)];
        val[0] = digits;
        memcpy(&val[1], hmac, sizeof(hmac));
        n = oath_tlv_put(out, tlv_cap, OATH_TAG_RESPONSE, val, (uint16_t)sizeof(val));
        memset(val, 0, sizeof(val));
    }
    memset(hmac, 0, sizeof(hmac));

    if (n == 0) {
        return oath_sw(out, cap, OATH_SW_FULL);
    }
    out[n]     = (uint8_t)(OATH_SW_OK >> 8);
    out[n + 1] = (uint8_t)(OATH_SW_OK & 0xFFu);
    return (uint16_t)(n + 2u);
}

/* ------------------------------------------------------------------ */
/* L'applet                                                            */
/* ------------------------------------------------------------------ */

/* Quelle opération l'écran doit annoncer. Quatre codes et non un seul : un
 * geste donné pour un code n'est pas un geste donné pour une destruction, et
 * c'est l'écran qui fait la différence (security/sec_confirm.h). */
static sec_op_t oath_sec_op(oath_touch_op_t op)
{
    switch (op) {
    case OATH_TOUCH_CALCULATE: return SEC_OP_OATH_CODE;
    case OATH_TOUCH_DELETE:    return SEC_OP_OATH_DELETE;
    case OATH_TOUCH_REPLACE:   return SEC_OP_OATH_REPLACE;
    case OATH_TOUCH_RESET:     return SEC_OP_OATH_RESET;
    /* Jamais atteint : oath_dispatch() ne rend OATH_SW_NEEDS_TOUCH que pour
     * les quatre ci-dessus. SEC_OP_UNKNOWN fait afficher « opération
     * inconnue » plutôt que le libellé d'une opération réelle — un écran qui
     * ment sur ce qu'on autorise est pire qu'un écran qui avoue ne pas
     * savoir. */
    default:                   return SEC_OP_UNKNOWN;
    }
}

/*
 * Appelé par le worker CCID (security/ccid.c), une commande à la fois. Peut
 * BLOQUER jusqu'à SEC_CONFIRM_TIMEOUT_MS : c'est voulu, et l'hôte est tenu au
 * courant par des trames WTX toutes les 1,5 s pendant ce temps. ykman n'a
 * aucune boucle de relance — sa méthode calculate() envoie l'APDU et attend la
 * réponse — donc c'est bien à la clé de tenir la commande ouverte.
 */
static uint16_t oath_apdu(const uint8_t *in, uint16_t in_len,
                          uint8_t *out, uint16_t cap)
{
    apdu_t cmd;
    if (!apdu_parse(in, in_len, &cmd)) {
        return oath_sw(out, cap, OATH_SW_WRONG_LENGTH);
    }

    const uint16_t n = oath_dispatch(&cmd, out, cap, &s_ctx);
    if (n != OATH_SW_NEEDS_TOUCH) {
        return n;
    }

    /*
     * L'étiquette que l'écran affichera sous le libellé d'opération. Pour un
     * RESET, il n'y a pas de compte à nommer : c'est le NOMBRE de comptes qui
     * part, mis en texte par oath_reset_label() — une fonction pure, parce que
     * cette chaîne traverse ensuite le même assainissement que n'importe quel
     * nom venu de l'hôte et doit être prouvée intacte à la sortie (voir
     * security/oath_name.h).
     */
    char reset_label[OATH_RESET_LABEL_MAX];
    const char *label;
    if (s_ctx.touch_op == OATH_TOUCH_RESET) {
        oath_reset_label(s_ctx.touch_count, reset_label, (uint8_t)sizeof(reset_label));
        label = reset_label;
    } else {
        /* Le nom du compte VISÉ, relu du magasin — pas celui de la trame :
         * c'est ce que la clé détient qui doit être montré, pas ce que l'hôte
         * en dit. NULL si le slot est vide, ce que sec_confirm_arm_named()
         * traite comme « rien à nommer ». */
        label = sec_store_label(s_ctx.touch_slot);
    }

    /*
     * L'APPUI. C'est le seul point du firmware où une destruction de secrets
     * OATH devient possible, et `accorde` ci-dessous est la seule chose qui
     * l'autorise. Ne jamais lui donner d'autre origine que ce retour — voir
     * l'avertissement en tête de ce fichier, et celui de oath_touch_commit()
     * dans security/oath_proto.h.
     *
     * L'appel BLOQUE le worker CCID, donc l'hôte : aucune commande ne peut
     * s'intercaler entre la demande et l'appui, et c'est cela qui rend
     * suffisant le simple index mémorisé dans `touch_slot`.
     */
    const bool accorde = (ccid_confirm_named(oath_sec_op(s_ctx.touch_op), label) == 1);

    if (s_ctx.touch_op == OATH_TOUCH_CALCULATE) {
        return oath_finish_calculate(accorde, out, cap);
    }
    return oath_touch_commit(&s_ctx, accorde, out, cap);
}

/* ------------------------------------------------------------------ */
/* Descripteurs et cycle de vie                                        */
/* ------------------------------------------------------------------ */

/*
 * ccid_init() force l'édition de liens de ccid.c — même raison exactement que
 * dans mode_pgp.c : ccid.c y définit usbd_app_driver_get_cb() en surcharge
 * FORTE d'un symbole FAIBLE de TinyUSB, et sans référence explicite
 * l'éditeur de liens laisserait gagner le symbole faible par défaut (aucun
 * pilote applicatif), donnant une interface CCID décrite mais non servie.
 * Idempotent, d'où l'appel dans les deux accesseurs (l'ordre d'évaluation des
 * arguments n'est pas spécifié en C).
 */
const uint8_t *mode_oath_fs_config(void)
{
    ccid_init();
    return s_fs_config;
}

const uint8_t *mode_oath_hs_config(void)
{
    ccid_init();
    return s_hs_config;
}

const char **mode_oath_strings(int *out_count)
{
    s_strings[STRID_SERIAL] = usb_device_serial();
    if (out_count != NULL) {
        *out_count = STRID_COUNT;
    }
    return s_strings;
}

void mode_oath_start(void)
{
    /*
     * Le contexte repart à zéro à chaque entrée : `selected` en particulier,
     * sans quoi un applet resté armé d'une session précédente accepterait des
     * PUT et des DELETE avant que l'hôte n'ait envoyé son SELECT.
     */
    memset(&s_ctx, 0, sizeof(s_ctx));

    /*
     * Le magasin est relu à CHAQUE entrée dans le mode, pas seulement au
     * premier boot. Même raison que mode_pgp_data_load() (voir son
     * commentaire) : le coffre ne charge rien tant qu'aucun mode ne l'a
     * demandé, et mettre des secrets TOTP en RAM au démarrage — alors
     * qu'aucune interface ne les sert — contredit ce principe. Ici s'y ajoute
     * que le magasin est PARTAGÉ avec le mode OTP : une session passée dans
     * l'autre mode a pu l'écrire.
     */
    sec_store_init();
    oath_salt_load(s_ctx.salt);
    ccid_set_applet(oath_apdu);
    ESP_LOGI(TAG, "applet OATH branché (%u compte(s) en magasin)",
             (unsigned)sec_store_count());
}

void mode_oath_stop(void)
{
    /*
     * Le worker d'abord : il peut être bloqué dans ccid_confirm_named(), à
     * poster des WTX sur une file que tud_deinit() s'apprête à détruire.
     * ccid_shutdown() ferme cette porte et attend qu'il ressorte. Une
     * confirmation en cours est REFUSÉE — aucun geste n'a eu lieu.
     */
    ccid_shutdown();

    /* Puis l'aiguillage, dans cet ordre : débrancher l'applet pendant que le
     * worker peut encore traiter une commande le ferait répondre en OpenPGP au
     * milieu d'une session OATH. */
    ccid_set_applet(NULL);

    /*
     * Et l'état s'efface. Ce n'est pas de la cosmétique : `touch_put_secret`
     * porte un secret EN CLAIR quand un PUT qui écrase attendait sa
     * confirmation au moment de la bascule. Il n'a rien à faire en RAM une
     * fois le mode quitté.
     */
    memset(&s_ctx, 0, sizeof(s_ctx));
}
