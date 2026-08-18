/* main/security/fido_key.c — derivation des cles FIDO2/U2F, en logique pure */
#include "fido_key.h"
#include <string.h>

/* Prefixes de separation de domaine — voir le commentaire en tete de
 * fido_key.h. NE JAMAIS les rendre identiques ni les retirer du message. */
#define FIDO_KEY_PREFIX_D   0x01u
#define FIDO_KEY_PREFIX_TAG 0x02u

#define FIDO_KEY_MSG_LEN (1u + 16u + 32u)   /* prefixe + nonce + rp_id_hash = 49 */

/* Construit le message HMAC : prefixe(1) || nonce(16) || rp_id_hash(32).
 * L'ordre suit la spec ; ce qui compte pour la separation de domaine, c'est
 * que le prefixe soit present ET que rp_id_hash fasse partie du message —
 * pas l'ordre relatif de nonce et rp_id_hash. */
static void build_message(uint8_t prefix, const uint8_t rp_id_hash[32],
                           const uint8_t nonce[16], uint8_t msg[FIDO_KEY_MSG_LEN])
{
    msg[0] = prefix;
    memcpy(msg + 1, nonce, 16);
    memcpy(msg + 17, rp_id_hash, 32);
}

void fido_key_derive(fido_hmac_fn hmac, const uint8_t rp_id_hash[32],
                     const uint8_t nonce[16], uint8_t d_out[32])
{
    if (!hmac || !rp_id_hash || !nonce || !d_out) return;
    uint8_t msg[FIDO_KEY_MSG_LEN];
    build_message(FIDO_KEY_PREFIX_D, rp_id_hash, nonce, msg);
    hmac(msg, sizeof(msg), d_out);
}

void fido_key_tag(fido_hmac_fn hmac, const uint8_t rp_id_hash[32],
                  const uint8_t nonce[16], uint8_t tag_out[16])
{
    if (!hmac || !rp_id_hash || !nonce || !tag_out) return;
    uint8_t msg[FIDO_KEY_MSG_LEN];
    uint8_t full[32];
    build_message(FIDO_KEY_PREFIX_TAG, rp_id_hash, nonce, msg);
    hmac(msg, sizeof(msg), full);
    /* Le tag est le HMAC tronque a 16 octets : suffisant comme
     * authentificateur (il ne sert qu'a verifier l'appartenance au domaine,
     * pas a signer), et ca garde le credential ID a 32 octets au total. */
    memcpy(tag_out, full, 16);
}

bool fido_key_check(fido_hmac_fn hmac, const uint8_t rp_id_hash[32],
                    const uint8_t cred_id[32])
{
    if (!hmac || !rp_id_hash || !cred_id) return false;

    const uint8_t *nonce  = cred_id;
    const uint8_t *tag_in = cred_id + 16;
    uint8_t expected[16];
    fido_key_tag(hmac, rp_id_hash, nonce, expected);

    /* Comparaison a temps constant : on accumule les differences par XOR sur
     * tous les octets au lieu de sortir au premier octet different, pour ne
     * pas laisser le temps de reponse reveler combien d'octets du tag sont
     * corrects a un attaquant qui rejoue des credential ID forges. */
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff = (uint8_t)(diff | (expected[i] ^ tag_in[i]));
    return diff == 0;
}
