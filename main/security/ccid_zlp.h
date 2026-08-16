#pragma once

/*
 * ccid_needs_zlp() — faut-il un paquet de longueur nulle après avoir livré une
 * réponse CCID sur l'endpoint bulk IN ?
 *
 * Seul morceau de logique pure de ccid.c, sorti dans cet en-tête pour être
 * testable sur l'hôte (test/test_ccid_zlp.c) : ccid.c lui-même ne compile
 * qu'avec TinyUSB. Divergence propre au coffre — voir la divergence BLOQUANT 2
 * en tête de main/security/ccid.c.
 *
 * USB 2.0 §5.8.3 : un transfert dont le compte d'octets est un multiple EXACT
 * du wMaxPacketSize ne porte pas de marqueur de fin implicite. La lecture bulk
 * de l'hôte (longueur demandée = dwMaxCCIDMessageLength = 271) ne se termine
 * que sur un paquet court OU une ZLP. Il faut donc en émettre une, et une
 * seule, dans ce cas précis.
 *
 * `mps` est le wMaxPacketSize RÉELLEMENT négocié, jamais une constante : 64 en
 * pleine vitesse, 512 en haute vitesse. Confondre les deux est le bug que ce
 * fichier existe pour empêcher — sur un endpoint à 512, une réponse de 64,
 * 128, 192 ou 256 octets est un paquet court, et la ZLP superflue figeait le
 * pipe CCID.
 */

#include <stdbool.h>
#include <stdint.h>

static inline bool ccid_needs_zlp(uint32_t xferred, uint16_t mps)
{
    /* xferred == 0 : c'est la ZLP elle-même qui vient d'être acquittée. En
     * enchaîner une seconde bouclerait sans fin sur l'endpoint IN. */
    if (xferred == 0u) {
        return false;
    }
    /* MPS inconnu (endpoint pas encore ouvert) : ne rien envoyer. Sauter une
     * ZLP fait au pire attendre l'hôte jusqu'à son timeout ; l'autre erreur —
     * ne pas réarmer OUT — fige le pipe pour de bon. Et ça écarte le modulo
     * par zéro. */
    if (mps == 0u) {
        return false;
    }
    return (xferred % (uint32_t)mps) == 0u;
}
