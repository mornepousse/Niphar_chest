/* main/security/cbor_enc.h — encodeur CBOR canonique, en logique pure.
 *
 * Sous-ensemble de CTAP2 : uniquement ce dont `authenticatorGetInfo` (tache
 * 5) a besoin — entiers, chaines d'octets, chaines de texte, en-tetes de
 * tableau et de map, booleens. Pas de decodeur ici : la tache 2 (contraintes
 * globales, ecart 2) reporte le decodage a `makeCredential`, le premier
 * consommateur reel.
 *
 * « Canonique » n'est pas un style : CBOR autorise plusieurs encodages du
 * meme entier (forme longue toujours valide), mais CTAP2 EXIGE la forme la
 * plus courte possible (RFC 8949 §4.2.1). Un encodeur qui emettrait
 * toujours 8 octets produirait du CBOR syntaxiquement valide que les
 * clients FIDO refuseraient — et l'echec se verrait sur du materiel, loin
 * d'ici. `cbor_uint` (et les en-tetes de longueur des autres types) choisit
 * donc toujours la forme la plus courte qui tient la valeur.
 *
 * Le tampon de sortie est fourni par l'appelant (jamais d'allocation ici,
 * meme discipline que ctaphid.c). Un debordement se SIGNALE via
 * `w->overflow` : il ne tronque jamais silencieusement une valeur, et
 * n'ecrit jamais au-dela de `cap`. Une fois `overflow` leve, il reste leve
 * (verrou) : tout appel ulterieur est un no-op, le tampon garde son
 * dernier etat coherent (que l'appelant doit de toute facon jeter).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    uint8_t *buf;       /* tampon de sortie, fourni par l'appelant */
    size_t   cap;       /* capacite totale du tampon */
    size_t   len;       /* octets ecrits jusqu'ici, toujours <= cap */
    bool     overflow;  /* leve des qu'une ecriture aurait depasse cap */
} cbor_w_t;

/* Remet l'encodeur a l'etat "tampon vide, aucun debordement". */
void cbor_w_init(cbor_w_t *w, uint8_t *buf, size_t cap);

/* Entier non signe (major type 0), forme la plus courte possible :
 * 1 octet jusqu'a 23, 2 octets (0x18) jusqu'a 255, 3 octets (0x19) jusqu'a
 * 65535, 5 octets (0x1A) jusqu'a 2^32-1, 9 octets (0x1B) au-dela. */
void cbor_uint(cbor_w_t *w, uint64_t v);

/* Chaine d'octets (major type 2) : en-tete de longueur (forme la plus
 * courte) suivi des `n` octets de `p`. */
void cbor_bytes(cbor_w_t *w, const uint8_t *p, size_t n);

/* Chaine de texte (major type 3) : en-tete de longueur (strlen(s)) suivi
 * des octets de `s`, sans le terminateur nul. */
void cbor_text(cbor_w_t *w, const char *s);

/* En-tete de tableau (major type 4) seul : les `n` elements qui suivent
 * sont a la charge de l'appelant, un a un. */
void cbor_array(cbor_w_t *w, size_t n);

/* En-tete de map (major type 5) seul : les `n` PAIRES cle/valeur qui
 * suivent sont a la charge de l'appelant. L'ordre canonique des cles (RFC
 * 8949 §4.2.1, requis par CTAP2) est une responsabilite de l'appelant —
 * cet encodeur ecrit les paires dans l'ordre ou on les lui donne. */
void cbor_map(cbor_w_t *w, size_t n);

/* Booleen (major type 7, valeurs simples 20/21) : un seul octet, 0xF4 pour
 * faux, 0xF5 pour vrai. */
void cbor_bool(cbor_w_t *w, bool b);
