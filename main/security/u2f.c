/* main/security/u2f.c — U2F (CTAP1) sur CTAPHID_MSG. Voir u2f.h pour le
 * contrat et pourquoi ce module est absent de test/.
 */
#include "u2f.h"

#include <string.h>
#include <stdbool.h>

#include "mbedtls/sha256.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/bignum.h"
#include "esp_random.h"
#include "esp_log.h"

#include "ecdsa_der.h"
#include "fido_attest.h"
#include "fido_key.h"
#include "fido_master.h"
#include "openpgp_crypto.h"
#include "sec_confirm.h"

static const char *TAG = "u2f";

/* INS U2F — FIDO U2F Raw Message Format v1.2, section 4. */
#define U2F_INS_REGISTER     0x01u
#define U2F_INS_AUTHENTICATE 0x02u
#define U2F_INS_VERSION      0x03u

/* Octet de controle P1 pour AUTHENTICATE (meme spec, section 4.3). */
#define U2F_P1_CHECK_ONLY       0x07u   /* sonder un key handle, sans authentifier */
#define U2F_P1_ENFORCE_PRESENCE 0x03u   /* authentifier reellement, presence exigee */

/* Status words ISO 7816-4 / U2F. */
#define U2F_SW_OK           0x9000u
#define U2F_SW_COND_NOT_SAT 0x6985u   /* conditions of use not satisfied (presence) */
#define U2F_SW_WRONG_DATA   0x6A80u   /* key handle inconnu / mal forme */
#define U2F_SW_WRONG_LENGTH 0x6700u
#define U2F_SW_WRONG_P1P2   0x6A86u
#define U2F_SW_INS_NOT_SUP  0x6D00u
#define U2F_SW_OTHER        0x6F00u   /* echec technique interne (signature/pubkey) */

/*
 * Slots sec_confirm dedies a U2F. Les familles CCID (ccid.c,
 * CCID_CONFIRM_SLOT) et OTP (otp_hid.c) ne coexistent JAMAIS avec FIDO — un
 * seul mode USB est installe a la fois (usb/usb_mode.c) — donc rien
 * n'exige que ces valeurs soient uniques a l'echelle du firmware. Elles
 * doivent seulement se distinguer L'UNE DE L'AUTRE : une confirmation armee
 * pour REGISTER ne doit jamais pouvoir autoriser un AUTHENTICATE arme en
 * meme temps (ou l'inverse), sur le meme modele que le controle
 * `slot == CCID_CONFIRM_SLOT` de ccid.c / `out_slot == s_armed_idx` de
 * otp_hid.c.
 */
#define U2F_SLOT_REGISTER 0xE0u
#define U2F_SLOT_AUTH     0xE1u

/* ------------------------------------------------------------------ */
/* Etat d'attente de confirmation — une requete a la fois              */
/* ------------------------------------------------------------------ */

/*
 * Le navigateur REJOUE la meme requete U2F (memes octets) toutes les
 * ~200 ms tant que l'utilisateur n'a pas touche le capteur : c'est le
 * mecanisme de sondage ("polling") prevu par la spec U2F, pas une erreur
 * cote hote. u2f_handle_apdu() tourne toujours sur usb_task (appelee
 * depuis tud_hid_set_report_cb via mode_fido.c/handle_message()) : memes
 * conditions que le commentaire de tete de sec_confirm.c pour
 * hook_confirm_arm()/hook_confirm_state() de otp_hid.c (arm()+poll() sur
 * UNE seule tache) — pas de verrou necessaire ici non plus.
 *
 * REARMER a chaque requete recue reinitialiserait le delai de
 * SEC_CONFIRM_TIMEOUT_MS a chaque sondage du navigateur, empechant tout
 * jamais un vrai timeout (sec_confirm_arm() ECRASE l'etat precedent — voir
 * sec_confirm.h). La regle retenue, sur le modele de otp_proto.c
 * (ST_IDLE/ST_WAIT) : armer UNE FOIS a la premiere requete pour un jeu de
 * parametres donne, puis se contenter de sondes (poll) sur les requetes
 * identiques suivantes ; une requete dont le contenu differe remplace
 * l'attente en cours (l'utilisateur ou le site a change d'avis).
 */
typedef enum {
    U2F_WAIT_NONE = 0,
    U2F_WAIT_REGISTER,
    U2F_WAIT_AUTH,
} u2f_wait_t;

static u2f_wait_t s_wait;
static uint8_t    s_wait_app[32];
static uint8_t    s_wait_chal[32];
static uint8_t    s_wait_nonce[16];     /* REGISTER : credential genere a l'armement */
static uint8_t    s_wait_cred_id[32];   /* AUTHENTICATE : key handle demande */

void u2f_reset(void)
{
    s_wait = U2F_WAIT_NONE;
    memset(s_wait_app, 0, sizeof(s_wait_app));
    memset(s_wait_chal, 0, sizeof(s_wait_chal));
    memset(s_wait_nonce, 0, sizeof(s_wait_nonce));
    memset(s_wait_cred_id, 0, sizeof(s_wait_cred_id));
}

/* ------------------------------------------------------------------ */
/* Construction de reponse — meme convention que respond()/sw_only()   */
/* dans openpgp_card.c : tronque au lieu d'echouer si `cap` est trop   */
/* juste, jamais de ecriture hors bornes.                              */
/* ------------------------------------------------------------------ */

static size_t respond(uint8_t *out, size_t cap, const uint8_t *data, size_t data_len, uint16_t sw)
{
    if (cap < 2u) return 0;
    if (data_len + 2u > cap) data_len = cap - 2u;
    if (data != NULL && data_len > 0u) memcpy(out, data, data_len);
    out[data_len]     = (uint8_t)(sw >> 8);
    out[data_len + 1] = (uint8_t)(sw & 0xFFu);
    return data_len + 2u;
}

static size_t sw_only(uint8_t *out, size_t cap, uint16_t sw)
{
    return respond(out, cap, NULL, 0, sw);
}

/* ------------------------------------------------------------------ */
/* U2F_VERSION — INS 0x03                                              */
/* ------------------------------------------------------------------ */

static size_t handle_version(uint8_t *out, size_t cap)
{
    static const uint8_t v[6] = { 'U', '2', 'F', '_', 'V', '2' };
    return respond(out, cap, v, sizeof(v), U2F_SW_OK);
}

/* ------------------------------------------------------------------ */
/* U2F_REGISTER — INS 0x01                                             */
/* ------------------------------------------------------------------ */

static size_t handle_register(const apdu_t *a, uint8_t *out, size_t cap, uint32_t now_ms)
{
    if (a->data == NULL || a->lc != 64u) {
        return sw_only(out, cap, U2F_SW_WRONG_LENGTH);
    }

    const uint8_t *challenge   = a->data;         /* 32 octets */
    const uint8_t *application = a->data + 32u;   /* 32 octets */

    const bool is_retry = (s_wait == U2F_WAIT_REGISTER)
                        && memcmp(s_wait_app, application, 32) == 0
                        && memcmp(s_wait_chal, challenge, 32) == 0;

    if (!is_retry) {
        /* Nouvelle requete : un nonce frais donne un credential frais, et
         * remplace toute attente precedente (REGISTER ou AUTHENTICATE) —
         * un seul message CTAP-HID en vol a la fois de toute facon (Ruling
         * 4, mode_fido.c), donc au plus une attente U2F a un instant donne. */
        esp_fill_random(s_wait_nonce, sizeof(s_wait_nonce));
        memcpy(s_wait_app, application, 32);
        memcpy(s_wait_chal, challenge, 32);
        s_wait = U2F_WAIT_REGISTER;
        sec_confirm_arm(U2F_SLOT_REGISTER, SEC_OP_FIDO_REGISTER, now_ms);
    }

    uint8_t              slot = 0;
    sec_confirm_state_t  st   = sec_confirm_poll(now_ms, &slot);
    const bool authorized = (st == SEC_CONFIRM_AUTHORIZED) && (slot == U2F_SLOT_REGISTER);

    if (!authorized) {
        /* PENDING (l'immense majorite des appels — pas d'appui possible sur
         * ce materiel, voir u2f.h), TIMEDOUT, ou un octroi vole par une
         * autre operation : dans tous les cas, pas de confirmation valide
         * pour CETTE requete. 0x6985 est exactement ce qu'un navigateur U2F
         * attend pour reessayer. */
        if (st == SEC_CONFIRM_TIMEDOUT
            || (st == SEC_CONFIRM_AUTHORIZED && !authorized)) {
            u2f_reset();   /* attente perimee : un nouveau geste devra rearmer */
        }
        return sw_only(out, cap, U2F_SW_COND_NOT_SAT);
    }

    /* Hisser le nonce dans un local AVANT u2f_reset() (C1, revue finale de
     * branche) : u2f_reset() zeroise s_wait_nonce, et les lignes qui
     * suivaient le lisaient APRES l'appel — toute derivation se faisait donc
     * sur seize octets nuls, silencieusement : la cle privee devenait une
     * fonction deterministe du seul `application`, et deux REGISTER sur le
     * meme site produisaient la MEME cle publique et le MEME key handle.
     * fido_key_check() valide ces identifiants sans broncher, ce qui a
     * masque le defaut jusqu'a la revue finale. */
    uint8_t nonce[16];
    memcpy(nonce, s_wait_nonce, sizeof(nonce));

    /* Consomme l'attente immediatement (que la suite reussisse ou non) —
     * une seule confirmation n'accorde qu'un seul REGISTER. */
    u2f_reset();

    uint8_t d[32];
    fido_key_derive(fido_master_hmac, application, nonce, d);

    uint8_t pubkey[65];
    const bool pk_ok = openpgp_crypto_p256_pubkey(d, pubkey);
    if (!pk_ok) {
        memset(d, 0, sizeof(d));
        memset(nonce, 0, sizeof(nonce));
        return sw_only(out, cap, U2F_SW_OTHER);
    }

    uint8_t tag[16];
    fido_key_tag(fido_master_hmac, application, nonce, tag);
    uint8_t cred_id[32];
    memcpy(cred_id, nonce, 16);
    memcpy(cred_id + 16, tag, 16);
    memset(nonce, 0, sizeof(nonce));   /* deja recopie dans cred_id ci-dessus */

    /* Message signe par la cle d'ATTESTATION (fido_attest_key — PAS `d`) :
     * 0x00 || application || challenge || keyHandle || pubkey — spec U2F
     * 4.3, "Registration Response Message : Success". */
    uint8_t msg[1u + 32u + 32u + 32u + 65u];
    size_t  off = 0;
    msg[off++] = 0x00u;
    memcpy(msg + off, application, 32); off += 32u;
    memcpy(msg + off, challenge, 32);   off += 32u;
    memcpy(msg + off, cred_id, 32);     off += 32u;
    memcpy(msg + off, pubkey, 65);      off += 65u;

    uint8_t hash[32];
    const bool hash_ok = mbedtls_sha256(msg, off, hash, 0) == 0;

    uint8_t sig[64];
    const bool sig_ok = hash_ok
                      && openpgp_crypto_p256_sign(fido_attest_key, hash, sizeof(hash), sig);
    memset(d, 0, sizeof(d));
    if (!sig_ok) {
        return sw_only(out, cap, U2F_SW_OTHER);
    }

    uint8_t sig_der[ECDSA_DER_MAX_LEN];
    const size_t sig_der_len = ecdsa_der_encode(sig, sig + 32, sig_der, sizeof(sig_der));
    memset(sig, 0, sizeof(sig));
    if (sig_der_len == 0u) {
        return sw_only(out, cap, U2F_SW_OTHER);
    }

    /* 0x05 || pubkey(65) || len(1) || keyHandle || cert || sig */
    uint8_t resp[1u + 65u + 1u + 32u + sizeof(fido_attest_cert_der) + ECDSA_DER_MAX_LEN];
    size_t  roff = 0;
    resp[roff++] = 0x05u;
    memcpy(resp + roff, pubkey, 65); roff += 65u;
    resp[roff++] = (uint8_t)sizeof(cred_id);   /* 32 : longueur du key handle */
    memcpy(resp + roff, cred_id, sizeof(cred_id)); roff += sizeof(cred_id);
    memcpy(resp + roff, fido_attest_cert_der, sizeof(fido_attest_cert_der));
    roff += sizeof(fido_attest_cert_der);
    memcpy(resp + roff, sig_der, sig_der_len); roff += sig_der_len;

    return respond(out, cap, resp, roff, U2F_SW_OK);
}

/* ------------------------------------------------------------------ */
/* U2F_AUTHENTICATE — INS 0x02                                         */
/* ------------------------------------------------------------------ */

static size_t handle_authenticate(const apdu_t *a, uint8_t *out, size_t cap, uint32_t now_ms)
{
    /* challenge(32) || application(32) || L(1) || keyHandle(L) — spec U2F
     * 4.3. Cette carte n'a jamais emis de key handle d'une longueur autre
     * que 32 (nonce(16) || tag(16), fido_key.h) : une longueur differente
     * ne peut donc pas etre l'un des notres, rejete comme WRONG_DATA plutot
     * que WRONG_LENGTH — la spec reserve ce dernier a une trame malformee,
     * pas a un key handle etranger. */
    if (a->data == NULL || a->lc < 65u) {
        return sw_only(out, cap, U2F_SW_WRONG_LENGTH);
    }

    const uint8_t *challenge   = a->data;
    const uint8_t *application = a->data + 32u;
    const uint8_t  handle_len  = a->data[64];

    if (handle_len != 32u || a->lc != (uint16_t)(65u + handle_len)) {
        return sw_only(out, cap, U2F_SW_WRONG_DATA);
    }
    const uint8_t *cred_id = a->data + 65u;

    if (!fido_key_check(fido_master_hmac, application, cred_id)) {
        /* Ni un key handle emis par cette carte, ni emis pour ce domaine —
         * spec U2F 4.3 : WRONG_DATA, dans les deux modes P1. */
        return sw_only(out, cap, U2F_SW_WRONG_DATA);
    }

    if (a->p1 == U2F_P1_CHECK_ONLY) {
        /* "check-only" ne authentifie JAMAIS reellement : un key handle
         * valide pour ce domaine repond TOUJOURS conditions-non-satisfaites
         * (spec U2F 4.3), meme si l'utilisateur vient de confirmer autre
         * chose. C'est un sondage ("ce key handle est-il le mien ?"), pas
         * une authentification — ne JAMAIS le faire aboutir a 0x9000 ici. */
        return sw_only(out, cap, U2F_SW_COND_NOT_SAT);
    }
    if (a->p1 != U2F_P1_ENFORCE_PRESENCE) {
        return sw_only(out, cap, U2F_SW_WRONG_P1P2);
    }

    const bool is_retry = (s_wait == U2F_WAIT_AUTH)
                        && memcmp(s_wait_app, application, 32) == 0
                        && memcmp(s_wait_chal, challenge, 32) == 0
                        && memcmp(s_wait_cred_id, cred_id, 32) == 0;

    if (!is_retry) {
        memcpy(s_wait_app, application, 32);
        memcpy(s_wait_chal, challenge, 32);
        memcpy(s_wait_cred_id, cred_id, 32);
        s_wait = U2F_WAIT_AUTH;
        sec_confirm_arm(U2F_SLOT_AUTH, SEC_OP_FIDO_AUTH, now_ms);
    }

    uint8_t              slot = 0;
    sec_confirm_state_t  st   = sec_confirm_poll(now_ms, &slot);
    const bool authorized = (st == SEC_CONFIRM_AUTHORIZED) && (slot == U2F_SLOT_AUTH);

    if (!authorized) {
        if (st == SEC_CONFIRM_TIMEDOUT
            || (st == SEC_CONFIRM_AUTHORIZED && !authorized)) {
            u2f_reset();
        }
        return sw_only(out, cap, U2F_SW_COND_NOT_SAT);
    }

    u2f_reset();

    /* nonce = les 16 premiers octets du key handle (fido_key.h) : cred_id
     * pointe deja dessus, pas besoin de copie separee. */
    uint8_t d[32];
    fido_key_derive(fido_master_hmac, application, cred_id, d);

    static const uint8_t counter[4] = { 0, 0, 0, 0 };   /* reste a 0 — decision de la spec (brief) */
    const uint8_t presence = 0x01u;

    /* Message signe : application || presence(1) || compteur(4) || challenge
     * — spec U2F 4.3, "Authentication Response Message : Success". */
    uint8_t msg[32u + 1u + 4u + 32u];
    size_t  off = 0;
    memcpy(msg + off, application, 32); off += 32u;
    msg[off++] = presence;
    memcpy(msg + off, counter, 4);      off += 4u;
    memcpy(msg + off, challenge, 32);   off += 32u;

    uint8_t hash[32];
    const bool hash_ok = mbedtls_sha256(msg, off, hash, 0) == 0;

    uint8_t sig[64];
    const bool sig_ok = hash_ok && openpgp_crypto_p256_sign(d, hash, sizeof(hash), sig);
    memset(d, 0, sizeof(d));
    if (!sig_ok) {
        return sw_only(out, cap, U2F_SW_OTHER);
    }

    uint8_t sig_der[ECDSA_DER_MAX_LEN];
    const size_t sig_der_len = ecdsa_der_encode(sig, sig + 32, sig_der, sizeof(sig_der));
    memset(sig, 0, sizeof(sig));
    if (sig_der_len == 0u) {
        return sw_only(out, cap, U2F_SW_OTHER);
    }

    /* presence(1) || compteur(4) || signature */
    uint8_t resp[1u + 4u + ECDSA_DER_MAX_LEN];
    size_t  roff = 0;
    resp[roff++] = presence;
    memcpy(resp + roff, counter, 4); roff += 4u;
    memcpy(resp + roff, sig_der, sig_der_len); roff += sig_der_len;

    return respond(out, cap, resp, roff, U2F_SW_OK);
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                             */
/* ------------------------------------------------------------------ */

size_t u2f_handle_apdu(const apdu_t *apdu, uint8_t *out, size_t cap, uint32_t now_ms)
{
    if (apdu == NULL || out == NULL) return 0;

    switch (apdu->ins) {
    case U2F_INS_VERSION:
        return handle_version(out, cap);
    case U2F_INS_REGISTER:
        return handle_register(apdu, out, cap, now_ms);
    case U2F_INS_AUTHENTICATE:
        return handle_authenticate(apdu, out, cap, now_ms);
    default:
        return sw_only(out, cap, U2F_SW_INS_NOT_SUP);
    }
}

/* ------------------------------------------------------------------ */
/* Autotest de démarrage — voir u2f.h pour le contrat complet.          */
/* ------------------------------------------------------------------ */

/*
 * Vérifie une signature r||s (brute, comme rendue par
 * openpgp_crypto_p256_sign()) contre `hash` et le point public non compressé
 * `pub65`, via mbedtls — même séquence que le KAT 1 de
 * openpgp_crypto_selftest() (openpgp_crypto.c). Rend faux sur n'importe quel
 * échec mbedtls, jamais un déréférencement ni un comportement indéfini.
 */
static bool verify_p256(const uint8_t pub65[65], const uint8_t hash[32],
                        const uint8_t sig[64])
{
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_mpi r, s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    bool ok = false;
    do {
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1)) break;
        if (mbedtls_ecp_point_read_binary(&grp, &Q, pub65, 65)) break;
        if (mbedtls_mpi_read_binary(&r, sig, 32)) break;
        if (mbedtls_mpi_read_binary(&s, sig + 32, 32)) break;
        ok = (mbedtls_ecdsa_verify(&grp, hash, 32, &Q, &r, &s) == 0);
    } while (0);

    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

bool u2f_selftest(void)
{
    /* Vecteurs FIXES, jamais des données d'hôte : un rp_id_hash et un nonce
     * arbitraires (fido_key_derive n'a pas de vecteur "spécial" à éviter),
     * et un message fixe à hacher/signer. Ces octets ne protègent rien —
     * seule leur PRÉSENCE (faire tourner la dérivation + la crypto au moins
     * une fois) compte ici, contrairement au K_maître (fido_master.c) ou à
     * fido_attest_key. */
    static const uint8_t rp_id_hash[32] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
        0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
    };
    static const uint8_t nonce[16] = {
        0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
        0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
    };
    static const char msg[] = "u2f_selftest fixed message";

    uint8_t hash[32];
    if (mbedtls_sha256((const uint8_t *)msg, strlen(msg), hash, 0) != 0) {
        ESP_LOGE(TAG, "selftest: sha256 a echoue");
        return false;
    }

    /* 1. Chemin CREDENTIAL : dérivation -> pubkey -> signature -> verif ->
     * encodage DER — exactement la séquence de handle_register()/
     * handle_authenticate() une fois la confirmation obtenue. */
    uint8_t d[32];
    fido_key_derive(fido_master_hmac, rp_id_hash, nonce, d);

    uint8_t pub[65];
    bool ok = openpgp_crypto_p256_pubkey(d, pub);
    if (!ok) {
        memset(d, 0, sizeof(d));
        ESP_LOGE(TAG, "selftest: derivation credential -> pubkey a echoue");
        return false;
    }

    uint8_t sig[64];
    ok = openpgp_crypto_p256_sign(d, hash, sizeof(hash), sig);
    memset(d, 0, sizeof(d));
    if (!ok) {
        ESP_LOGE(TAG, "selftest: signature credential a echoue");
        return false;
    }

    if (!verify_p256(pub, hash, sig)) {
        memset(sig, 0, sizeof(sig));
        ESP_LOGE(TAG, "selftest: verification credential a echoue (signature/pubkey incoherents)");
        return false;
    }

    uint8_t der[ECDSA_DER_MAX_LEN];
    size_t  der_len = ecdsa_der_encode(sig, sig + 32, der, sizeof(der));
    memset(sig, 0, sizeof(sig));
    if (der_len < 8u || der[0] != 0x30u || der[1] != (uint8_t)(der_len - 2u)) {
        ESP_LOGE(TAG, "selftest: encodage DER credential incoherent (len=%u)", (unsigned)der_len);
        return false;
    }

    /* 2. Chemin ATTESTATION : la clé qui signe RÉELLEMENT U2F_REGISTER
     * (fido_attest_key, pas `d`) — vérifiée contre fido_attest_pubkey,
     * extrait du certificat (fido_attest.h). Un échec ici indiquerait soit
     * une régression mbedtls, soit — plus grave — un fido_attest_key/
     * fido_attest_pubkey/fido_attest_cert_der devenus incohérents entre eux
     * (voir le commentaire de fido_attest.h sur leur régénération conjointe). */
    ok = openpgp_crypto_p256_sign(fido_attest_key, hash, sizeof(hash), sig);
    if (!ok) {
        memset(sig, 0, sizeof(sig));
        ESP_LOGE(TAG, "selftest: signature attestation a echoue");
        return false;
    }
    if (!verify_p256(fido_attest_pubkey, hash, sig)) {
        memset(sig, 0, sizeof(sig));
        ESP_LOGE(TAG, "selftest: verification attestation a echoue (cle/certificat incoherents)");
        return false;
    }
    memset(sig, 0, sizeof(sig));

    ESP_LOGI(TAG, "selftest: PASS (credential + attestation)");
    return true;
}
