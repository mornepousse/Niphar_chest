#include "test_framework.h"
#include "fido_key.h"
#include <string.h>

/* ---- fake_hmac : Ruling 1 de la tache 6 ----
 *
 * fido_hmac_fn est injectee precisement pour ne PAS dependre de mbedtls sur
 * l'hote. Ce que ces tests eprouvent est la STRUCTURE de la derivation —
 * separation de domaine, rejet d'un identifiant d'un autre domaine — pas la
 * correction cryptographique de HMAC-SHA256, qui n'a pas sa place ici.
 *
 * Mais un faux HMAC trop simple masquerait un vrai defaut : s'il ignorait
 * une partie de son message (par ex. ne regardait que le premier octet), le
 * test de separation de domaine passerait meme si fido_key.c oubliait de
 * mettre rp_id_hash dans le message HMAC reel — exactement la mutation
 * "rp_id_hash retire du message HMAC" qu'on doit pouvoir detecter plus bas.
 *
 * Ce faux HMAC doit donc etre sensible a CHAQUE octet du message, a
 * n'importe quelle position. Ce n'est PAS un HMAC cryptographique — pas
 * d'avalanche garantie, pas de resistance aux collisions — seulement une
 * fonction deterministe ou changer un seul octet, n'importe ou dans `msg`,
 * change la sortie. C'est prouve explicitement par les deux tests dedies
 * ci-dessous (premier octet, et octet de queue — la position du rp_id_hash).
 */
static void fake_hmac(const uint8_t *msg, size_t len, uint8_t out[32])
{
    memset(out, 0, 32);
    uint32_t acc = 0x9E3779B9u ^ (uint32_t)len;
    for (size_t i = 0; i < len; i++) {
        acc = acc * 2654435761u + msg[i] + (uint32_t)i;
        acc ^= acc >> 15;
        /* Chaque octet du message remue trois positions de sortie distinctes
         * avec des derives differentes de `acc`, pas une seule case qui lui
         * serait dediee : un octet en position i>32 ne se contente donc pas
         * d'ecraser silencieusement un octet deja pose par un octet i-32. */
        out[i % 32]        ^= (uint8_t)(acc);
        out[(i + 7) % 32]  ^= (uint8_t)(acc >> 8);
        out[(i + 19) % 32] ^= (uint8_t)(acc >> 16);
    }
}

/* ---- preuve que fake_hmac ne masque rien ---- */

/* Deux messages ne differant QUE par leur premier octet (c'est exactement
 * le prefixe 0x01/0x02 de separation de domaine) doivent produire des
 * sorties differentes. Sans cette garantie, le test de separation de
 * domaine plus bas pourrait passer meme si fido_key_derive/fido_key_tag
 * oubliaient le prefixe. */
static void test_fake_hmac_is_sensitive_to_first_byte(void)
{
    uint8_t msg_a[8] = {0x01, 1, 2, 3, 4, 5, 6, 7};
    uint8_t msg_b[8] = {0x02, 1, 2, 3, 4, 5, 6, 7};
    uint8_t out_a[32], out_b[32];
    fake_hmac(msg_a, sizeof(msg_a), out_a);
    fake_hmac(msg_b, sizeof(msg_b), out_b);
    TEST_ASSERT(memcmp(out_a, out_b, 32) != 0,
                "fake_hmac distingue deux messages ne differant que par leur premier octet");
}

/* Meme preuve, mais sur le DERNIER octet du message a la taille exacte du
 * message reel de fido_key.c (49 octets : 1 prefixe + 16 nonce + 32
 * rp_id_hash) — le dernier octet est le dernier octet de rp_id_hash. Si
 * fake_hmac ignorait la queue du message, une implementation qui tronquerait
 * rp_id_hash resterait invisible aux tests de separation/rejet ci-dessous. */
static void test_fake_hmac_is_sensitive_to_last_byte_of_full_length_message(void)
{
    uint8_t msg_a[49]; memset(msg_a, 0xAA, sizeof(msg_a));
    uint8_t msg_b[49]; memset(msg_b, 0xAA, sizeof(msg_b));
    msg_b[48] ^= 0xFFu;   /* seul le dernier octet change */
    uint8_t out_a[32], out_b[32];
    fake_hmac(msg_a, sizeof(msg_a), out_a);
    fake_hmac(msg_b, sizeof(msg_b), out_b);
    TEST_ASSERT(memcmp(out_a, out_b, 32) != 0,
                "fake_hmac distingue deux messages de 49 octets ne differant que sur le dernier");
}

/* ---- les deux tests du cahier des charges, repris verbatim ---- */

/* LA SEPARATION DE DOMAINE. Sans les octets de prefixe 0x01/0x02,
 * l'authentificateur du credential SERAIT sa cle privee — et le publier dans
 * le credential ID reviendrait a publier la cle. Ce test est la seule chose
 * qui tient cette propriete. */
static void test_key_and_tag_are_different_derivations(void)
{
    uint8_t rp[32]; memset(rp,0xAB,32);
    uint8_t nonce[16]; memset(nonce,0xCD,16);
    uint8_t d[32], tag[16];
    fido_key_derive(fake_hmac, rp, nonce, d);
    fido_key_tag(fake_hmac, rp, nonce, tag);
    TEST_ASSERT(memcmp(d, tag, 16) != 0,
                "la cle privee et son authentificateur ne partagent PAS de prefixe");
}

/* Un credential ID forge pour un AUTRE domaine doit etre rejete : c'est ce qui
 * empeche un site de rejouer l'identifiant obtenu sur un autre. */
static void test_credential_of_another_rp_is_rejected(void)
{
    uint8_t rp_a[32]; memset(rp_a,0x11,32);
    uint8_t rp_b[32]; memset(rp_b,0x22,32);
    uint8_t nonce[16]; memset(nonce,0x33,16);
    uint8_t cred[32]; memcpy(cred,nonce,16);
    fido_key_tag(fake_hmac, rp_a, nonce, cred+16);
    TEST_ASSERT(fido_key_check(fake_hmac, rp_a, cred),  "accepte sur son domaine");
    TEST_ASSERT(!fido_key_check(fake_hmac, rp_b, cred), "refuse sur un autre domaine");
}

/* ---- couverture complementaire ---- */

/* La derivation est une fonction pure : mêmes entrées, même sortie, sur deux
 * appels independants. Sans ce test, un bug d'etat cache (variable statique
 * mutee) resterait invisible. */
static void test_derive_is_deterministic(void)
{
    uint8_t rp[32]; memset(rp,0x55,32);
    uint8_t nonce[16]; memset(nonce,0x66,16);
    uint8_t d1[32], d2[32];
    fido_key_derive(fake_hmac, rp, nonce, d1);
    fido_key_derive(fake_hmac, rp, nonce, d2);
    TEST_ASSERT(memcmp(d1, d2, 32) == 0, "meme entree, meme cle privee derivee");
}

/* Deux nonces distincts, meme domaine : deux cles privees distinctes. C'est
 * ce qui rend chaque credential imprevisible malgre une cle maitresse
 * partagee. */
static void test_derive_differs_by_nonce(void)
{
    uint8_t rp[32]; memset(rp,0x77,32);
    uint8_t nonce_a[16]; memset(nonce_a,0x01,16);
    uint8_t nonce_b[16]; memset(nonce_b,0x02,16);
    uint8_t d_a[32], d_b[32];
    fido_key_derive(fake_hmac, rp, nonce_a, d_a);
    fido_key_derive(fake_hmac, rp, nonce_b, d_b);
    TEST_ASSERT(memcmp(d_a, d_b, 32) != 0, "deux nonces donnent deux cles distinctes");
}

/* Meme nonce, deux domaines distincts : deux cles privees distinctes. Sans
 * ce test, une implementation qui oublierait rp_id_hash du message HMAC (la
 * mutation obligatoire de l'etape 4) laisserait ce test passer a tort si
 * seul test_credential_of_another_rp_is_rejected existait — celui-ci isole
 * fido_key_derive de fido_key_check. */
static void test_derive_differs_by_rp_id_hash(void)
{
    uint8_t rp_a[32]; memset(rp_a,0x88,32);
    uint8_t rp_b[32]; memset(rp_b,0x99,32);
    uint8_t nonce[16]; memset(nonce,0x44,16);
    uint8_t d_a[32], d_b[32];
    fido_key_derive(fake_hmac, rp_a, nonce, d_a);
    fido_key_derive(fake_hmac, rp_b, nonce, d_b);
    TEST_ASSERT(memcmp(d_a, d_b, 32) != 0, "deux domaines donnent deux cles distinctes");
}

/* Un credential ID dont le tag a ete altere (un seul octet) est rejete sur
 * SON PROPRE domaine — distinct du test "autre domaine" ci-dessus, qui ne
 * touche pas au tag. */
static void test_tampered_tag_is_rejected(void)
{
    uint8_t rp[32]; memset(rp,0xEE,32);
    uint8_t nonce[16]; memset(nonce,0xFA,16);
    uint8_t cred[32]; memcpy(cred,nonce,16);
    fido_key_tag(fake_hmac, rp, nonce, cred+16);
    TEST_ASSERT(fido_key_check(fake_hmac, rp, cred), "accepte avant alteration");
    cred[31] ^= 0x01u;   /* un seul bit du dernier octet du tag */
    TEST_ASSERT(!fido_key_check(fake_hmac, rp, cred), "refuse des qu'un octet du tag differe");
}

void test_fido_key(void)
{
    TEST_SUITE("fido_key derivation");
    TEST_RUN(test_fake_hmac_is_sensitive_to_first_byte);
    TEST_RUN(test_fake_hmac_is_sensitive_to_last_byte_of_full_length_message);
    TEST_RUN(test_key_and_tag_are_different_derivations);
    TEST_RUN(test_credential_of_another_rp_is_rejected);
    TEST_RUN(test_derive_is_deterministic);
    TEST_RUN(test_derive_differs_by_nonce);
    TEST_RUN(test_derive_differs_by_rp_id_hash);
    TEST_RUN(test_tampered_tag_is_rejected);
}
