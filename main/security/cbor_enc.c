/* main/security/cbor_enc.c — encodeur CBOR canonique, en logique pure */
#include "cbor_enc.h"
#include <string.h>

/*
 * Reserve `n` octets a la fin du tampon et fait avancer `w->len` d'autant.
 * Rend `false` sans rien ecrire ni faire avancer `len` si `n` ne tient pas
 * dans la place restante — et leve `overflow` dans ce cas. Un champ
 * (en-tete ou charge utile) s'ecrit donc en entier ou pas du tout : jamais
 * une moitie de champ, ce qui est precisement ce qui distinguerait un CBOR
 * tronque (faux mais plausible) d'un debordement signale.
 *
 * Une fois `overflow` leve, il reste leve : l'appel qui suit rend `false`
 * immediatement, sans reexaminer la capacite. Le tampon garde son dernier
 * etat coherent, que l'appelant est de toute facon tenu de jeter.
 */
static bool cbor_reserve(cbor_w_t *w, size_t n, uint8_t **out)
{
    if (w->overflow) return false;
    if (n > w->cap - w->len) {
        w->overflow = true;
        return false;
    }
    *out = w->buf + w->len;
    w->len += n;
    return true;
}

/* Copie `n` octets de `p` a la suite du tampon, ou signale le debordement
 * via cbor_reserve sans rien ecrire. */
static void w_put(cbor_w_t *w, const uint8_t *p, size_t n)
{
    uint8_t *dst;
    if (!cbor_reserve(w, n, &dst)) return;
    if (n) memcpy(dst, p, n);
}

/*
 * En-tete CBOR pour le type majeur `major` (0-7) et l'argument `v` — c'est
 * `v` lui-meme pour un entier (major type 0), la longueur pour une chaine
 * ou un tableau/map (major types 2-5). Forme la plus courte possible parmi
 * les cinq prevues par RFC 8949 §3 : l'argument tient dans les 5 bits bas
 * du premier octet jusqu'a 23 ; au-dela, le premier octet porte 24/25/26/27
 * (0x18-0x1B) et l'argument suit en gros-boutiste sur 1/2/4/8 octets.
 */
static void cbor_write_header(cbor_w_t *w, uint8_t major, uint64_t v)
{
    uint8_t mt = (uint8_t)(major << 5);

    if (v <= 23u) {
        uint8_t b = (uint8_t)(mt | (uint8_t)v);
        w_put(w, &b, 1);
    } else if (v <= 0xFFu) {
        uint8_t b[2] = { (uint8_t)(mt | 0x18u), (uint8_t)v };
        w_put(w, b, sizeof b);
    } else if (v <= 0xFFFFu) {
        uint8_t b[3] = { (uint8_t)(mt | 0x19u), (uint8_t)(v >> 8), (uint8_t)v };
        w_put(w, b, sizeof b);
    } else if (v <= 0xFFFFFFFFu) {
        uint8_t b[5] = {
            (uint8_t)(mt | 0x1Au),
            (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v
        };
        w_put(w, b, sizeof b);
    } else {
        uint8_t b[9];
        b[0] = (uint8_t)(mt | 0x1Bu);
        for (int i = 0; i < 8; i++) {
            b[1 + i] = (uint8_t)(v >> (56 - 8 * i));
        }
        w_put(w, b, sizeof b);
    }
}

void cbor_w_init(cbor_w_t *w, uint8_t *buf, size_t cap)
{
    if (!w) return;
    w->buf      = buf;
    w->cap      = cap;
    w->len      = 0;
    w->overflow = false;
}

void cbor_uint(cbor_w_t *w, uint64_t v)
{
    if (!w) return;
    cbor_write_header(w, 0, v);
}

void cbor_bytes(cbor_w_t *w, const uint8_t *p, size_t n)
{
    if (!w) return;
    /* Meme convention que cbor_text() : un pointeur nul vaut longueur nulle,
     * jamais un memcpy(dst, NULL, n) — comportement indefini des que n>0
     * (revue finale de branche, mineur porte depuis la tache 4). */
    if (p == NULL) n = 0;
    cbor_write_header(w, 2, (uint64_t)n);
    w_put(w, p, n);
}

void cbor_text(cbor_w_t *w, const char *s)
{
    if (!w) return;
    size_t n = s ? strlen(s) : 0;
    cbor_write_header(w, 3, (uint64_t)n);
    w_put(w, (const uint8_t *)s, n);
}

void cbor_array(cbor_w_t *w, size_t n)
{
    if (!w) return;
    cbor_write_header(w, 4, (uint64_t)n);
}

void cbor_map(cbor_w_t *w, size_t n)
{
    if (!w) return;
    cbor_write_header(w, 5, (uint64_t)n);
}

void cbor_bool(cbor_w_t *w, bool b)
{
    if (!w) return;
    uint8_t v = b ? 0xF5u : 0xF4u;
    w_put(w, &v, 1);
}
