/* main/security/ecdsa_der.c — encodage DER d'une signature ECDSA P-256, en
 * logique pure. Voir ecdsa_der.h pour le contrat.
 */
#include "ecdsa_der.h"
#include <string.h>
#include <stdbool.h>

/*
 * Encode un entier non signe de 32 octets big-endian en INTEGER DER dans
 * `out` (capacite `cap`). Retire les zeros de tete (mais garde toujours au
 * moins un octet : la valeur zero s'encode en un seul octet 0x00), et
 * prefixe un 0x00 de bourrage si l'octet de poids fort restant a son bit de
 * signe pose (>= 0x80) — sinon un DER INTEGER lirait cette valeur comme
 * negative. Rend le nombre d'octets ecrits, ou 0 si `cap` est insuffisant.
 */
static size_t encode_integer(const uint8_t v[32], uint8_t *out, size_t cap)
{
    size_t skip = 0;
    while (skip < 31u && v[skip] == 0x00u) skip++;

    const size_t len = 32u - skip;
    const bool   pad = (v[skip] & 0x80u) != 0u;
    const size_t total = 2u + (pad ? 1u : 0u) + len;
    if (total > cap) return 0;

    out[0] = 0x02u;   /* tag INTEGER */
    out[1] = (uint8_t)(len + (pad ? 1u : 0u));
    size_t off = 2u;
    if (pad) out[off++] = 0x00u;
    memcpy(out + off, v + skip, len);
    return off + len;
}

size_t ecdsa_der_encode(const uint8_t r[32], const uint8_t s[32],
                        uint8_t *out, size_t cap)
{
    if (r == NULL || s == NULL || out == NULL) return 0;

    /*
     * Le contenu (les deux INTEGER) est d'abord ecrit dans un tampon local
     * borne exactement : l'en-tete SEQUENCE, qui precede son propre
     * contenu, a besoin de connaitre sa longueur totale avant d'etre ecrit.
     * 2 x (tag 1 + longueur 1 + 32 octets + bourrage eventuel 1) = 70 au
     * plus — voir ECDSA_DER_MAX_LEN.
     */
    uint8_t body[2u * (2u + 1u + 32u)];
    size_t off = encode_integer(r, body, sizeof(body));
    if (off == 0u) return 0;
    const size_t n = encode_integer(s, body + off, sizeof(body) - off);
    if (n == 0u) return 0;
    off += n;

    /* Ne peut pas arriver avec des entrees de 32 octets (off <= 70), mais
     * la forme longue de longueur DER n'est pas geree ici : mieux vaut
     * refuser explicitement qu'ecrire une longueur fausse si cette
     * hypothese devait un jour changer. */
    if (off > 127u) return 0;

    const size_t total = 2u + off;
    if (total > cap) return 0;

    out[0] = 0x30u;   /* tag SEQUENCE */
    out[1] = (uint8_t)off;
    memcpy(out + 2, body, off);
    return total;
}
