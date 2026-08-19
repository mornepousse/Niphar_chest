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
    /* Sous OATH_NAME_OUT_SZ_MIN, il n'y a la place ni de l'empreinte, ni du
     * marqueur, ni du terminateur ensemble : sans cette garde,
     * `visibles = out_sz - 1` et `prefixe = visibles - 2` bouclent (uint8_t)
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

    /*
     * Le nom COMPLET, jusqu'a son dernier octet. Une version precedente
     * coupait au premier ':' pour ne garder que l'emetteur : voir la regle 1
     * de l'en-tete pour pourquoi ce n'est plus le cas — deux comptes chez le
     * meme emetteur (« OVH:perso » et « OVH:pro ») rendaient la meme chaine.
     */
    const uint16_t fin = raw_len;

    /* Budget TOTAL de caracteres dessines : tout ce que out_sz peut porter
     * hors terminateur. Le marqueur de troncature et l'empreinte se prennent
     * DEDANS et non au-dela — un vingt-deuxieme caractere serait ampute par
     * fb_set_pixel() (voir OATH_NAME_DISPLAY_MAX), et l'ampute serait
     * justement le marqueur. */
    const uint8_t visibles = (uint8_t)(out_sz - 1);
    const uint16_t reste = (uint16_t)(fin - i);
    const uint16_t debut = i;   /* debut du nom, avant que la boucle de
                                  * troncature ne fasse avancer i : l'empreinte
                                  * doit porter sur le nom ENTIER (voir plus
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

    /* Troncature. Une simple coupe rendrait identiques deux noms qui ne
     * divergent qu'au-dela — un environnement suffixe en chiffre
     * (« ServiceProd1 » / « ServiceProd2 »), ou n'importe quel nom dont les
     * premiers caracteres coincident. L'avant-dernier caractere dessine porte
     * donc une empreinte du nom entier au lieu d'etre un caractere litteral de
     * plus : deux noms qui ne different qu'apres la coupe, OU qui ne different
     * que par un octet confondu avec un autre lors de l'assainissement du
     * prefixe visible (voir plus bas), restent distinguables a l'ecran.
     *
     * DEUX de moins que `visibles`, pas un : l'empreinte ET le marqueur se
     * prennent tous deux dans le budget dessine. */
    const uint8_t prefixe = (uint8_t)(visibles - 2);
    uint8_t n = 0;
    for (; n < prefixe; i++, n++) {
        unsigned char c = (unsigned char)raw[i];
        out[n] = imprimable(c) ? majuscule((char)c) : '?';
    }
    /* L'empreinte porte sur le nom ENTIER (depuis `debut`), pas seulement
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

void oath_reset_label(uint8_t count, char *out, uint8_t out_sz)
{
    if (out == NULL || out_sz == 0) return;
    out[0] = '\0';

    /*
     * « COMPTE » au singulier pour zero comme pour un : c'est l'accord
     * francais, et « 0 COMPTES » se lirait comme une faute de plus a un
     * moment ou l'ecran doit inspirer confiance. Ecrit sans <stdio.h> :
     * snprintf tire tout le formateur de la libc dans le firmware pour trois
     * chiffres et un mot.
     */
    const char *mot = (count <= 1u) ? " COMPTE" : " COMPTES";

    /* Chiffres a l'envers, puis retournes : jusqu'a trois pour un uint8_t. */
    char chiffres[3];
    uint8_t nb = 0;
    uint8_t v = count;
    do {
        chiffres[nb++] = (char)('0' + (v % 10u));
        v = (uint8_t)(v / 10u);
    } while (v != 0u);

    uint16_t lm = 0;
    while (mot[lm] != '\0') lm++;

    /* Refus franc plutot que troncature : « 1 » a la place de « 16 » ferait
     * autoriser l'effacement de seize comptes en croyant en effacer un. La
     * chaine vide, elle, laisse l'ecran sur le seul libelle d'operation
     * (« RESET OATH ») — moins informatif, mais jamais faux. */
    if ((uint16_t)nb + lm + 1u > (uint16_t)out_sz) return;

    uint8_t n = 0;
    for (uint8_t k = nb; k > 0u; k--) out[n++] = chiffres[k - 1u];
    for (uint16_t k = 0; k < lm; k++) out[n++] = mot[k];
    out[n] = '\0';
}
