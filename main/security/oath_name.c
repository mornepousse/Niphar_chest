#include "oath_name.h"

#include <stdbool.h>
#include <string.h>

/* Majuscule ASCII sans <ctype.h> : toupper() depend de la locale, et une
 * locale turque transformerait « i » en « İ ». */
static char majuscule(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* Imprimable ASCII strict. Tout le reste — controle, UTF-8, octet haut —
 * devient « ? » : voir la regle 2 de l'en-tete. */
static bool imprimable(unsigned char c)
{
    return c >= 0x20 && c <= 0x7E;
}

void oath_name_display(const char *raw, uint16_t raw_len, char *out, uint8_t out_sz)
{
    if (out == NULL || out_sz == 0) return;
    out[0] = '\0';
    if (raw == NULL || raw_len == 0) return;

    /* Prefixe de periode : « 30/GitHub:mae ». Retire s'il est present ET suivi
     * d'un '/' — un nom qui commence par des chiffres sans '/' reste entier. */
    uint16_t i = 0;
    uint16_t chiffres = 0;
    while (chiffres < raw_len && raw[chiffres] >= '0' && raw[chiffres] <= '9') chiffres++;
    if (chiffres > 0 && chiffres < raw_len && raw[chiffres] == '/') i = (uint16_t)(chiffres + 1);

    /* Issuer = jusqu'au premier ':'. Absent -> tout le reste. */
    uint16_t fin = i;
    while (fin < raw_len && raw[fin] != ':') fin++;

    const uint8_t visibles = (uint8_t)(out_sz - 2);   /* place le marqueur et le \0 */
    const uint16_t reste = (uint16_t)(fin - i);

    if (reste <= visibles) {
        /* Tient en entier : pas de marqueur, chaque caractere visible. */
        uint8_t n = 0;
        for (; i < fin; i++, n++) {
            unsigned char c = (unsigned char)raw[i];
            out[n] = imprimable(c) ? majuscule((char)c) : '?';
        }
        out[n] = '\0';
        return;
    }

    /* Troncature. Une simple coupe au dixieme caractere rendrait identiques
     * deux issuers qui ne divergent qu'au-dela — un environnement suffixe en
     * chiffre (« ServiceProd1 » / « ServiceProd2 »), ou n'importe quel nom
     * dont les dix premiers caracteres coincident. Le dernier caractere
     * visible porte donc une empreinte de la partie masquee au lieu d'etre
     * un simple caractere de plus : deux noms qui ne different qu'apres le
     * dixieme caractere restent distinguables a l'ecran. */
    const uint8_t prefixe = (uint8_t)(visibles - 1);
    uint8_t n = 0;
    for (; n < prefixe; i++, n++) {
        unsigned char c = (unsigned char)raw[i];
        out[n] = imprimable(c) ? majuscule((char)c) : '?';
    }
    uint32_t empreinte = 0;
    for (uint16_t k = i; k < fin; k++) empreinte = empreinte * 33u + (unsigned char)raw[k];
    uint8_t idx = (uint8_t)(empreinte % 36u);
    out[n++] = (char)(idx < 10 ? ('0' + idx) : ('A' + (idx - 10)));
    out[n++] = '?';   /* marqueur : il reste du nom non montre */
    out[n] = '\0';
}
