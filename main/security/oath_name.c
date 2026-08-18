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
    /* Sous OATH_NAME_OUT_SZ_MIN, il n'y a la place ni d'un caractere utile,
     * ni du marqueur, ni du terminateur ensemble : sans cette garde,
     * `visibles = out_sz - 2` et `prefixe = visibles - 1` bouclent (uint8_t)
     * et la branche de troncature ecrit bien au-dela de out_sz. La chaine
     * vide est la seule sortie qu'on puisse garantir sans deborder. */
    if (out_sz < OATH_NAME_OUT_SZ_MIN) return;
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
    const uint16_t debut = i;   /* debut de l'issuer, avant que la boucle de
                                  * troncature ne fasse avancer i : l'empreinte
                                  * doit porter sur l'issuer ENTIER (voir plus
                                  * bas), pas seulement sur sa queue masquee. */

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
     * visible porte donc une empreinte de l'issuer entier au lieu d'etre un
     * simple caractere de plus : deux noms qui ne different qu'apres le
     * dixieme caractere, OU qui ne different que par un octet confondu avec
     * un autre lors de l'assainissement du prefixe visible (voir plus bas),
     * restent distinguables a l'ecran. */
    const uint8_t prefixe = (uint8_t)(visibles - 1);
    uint8_t n = 0;
    for (; n < prefixe; i++, n++) {
        unsigned char c = (unsigned char)raw[i];
        out[n] = imprimable(c) ? majuscule((char)c) : '?';
    }
    /* L'empreinte porte sur l'issuer ENTIER (depuis `debut`), pas seulement
     * sur la queue masquee a partir d'ici : deux octets de controle distincts
     * (« \x01 » vs « \x02 ») retombent tous deux sur '?' dans le prefixe
     * visible, et si l'empreinte ignorait la partie deja affichee, deux noms
     * identiques hors ce seul octet — meme prefixe assaini, meme queue —
     * produiraient la meme sortie. Sur les octets BRUTS, avant assainissement :
     * c'est ce qui distingue « \x01 » de « \x02 », que imprimable() confond. */
    /* Base 31, pas 33 : pgcd(33,36)=3 annule structurellement toute paire
     * d'octets dont la difference est un multiple de 3 (une classe entiere,
     * pas un hasard de collision) une fois reduit modulo 36. pgcd(31,36)=1 —
     * chaque poids 31^k mod 36 est inversible, donc une difference d'un seul
     * octet ne peut plus s'annuler a elle seule. */
    uint32_t empreinte = 0;
    for (uint16_t k = debut; k < fin; k++) empreinte = empreinte * 31u + (unsigned char)raw[k];
    uint8_t idx = (uint8_t)(empreinte % 36u);
    out[n++] = (char)(idx < 10 ? ('0' + idx) : ('A' + (idx - 10)));
    out[n++] = '?';   /* marqueur : il reste du nom non montre */
    out[n] = '\0';
}
