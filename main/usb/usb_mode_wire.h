#pragma once

/*
 * usb_mode_wire — la traduction entre les valeurs DE FIL du lien S3↔coffre et
 * l'énumération interne usb_mode_t.
 *
 * Morceau PUR, sorti pour le test hôte sur le modèle de usb_mode_state.h et de
 * usb_mode_cycle.h : usb_mode.c lui-même ne compile qu'avec ESP-IDF.
 *
 * POURQUOI DEUX NUMÉROTATIONS ALORS QU'ELLES COÏNCIDENT. Les valeurs de fil
 * sont un contrat publié à KeSp (docs/LINK_CONTRACT.md) ; usb_mode_t est une
 * énumération interne que rien n'interdit de réordonner — pour insérer un mode
 * entre deux autres, pour grouper les personnalités CCID. Le jour où ça
 * arrivera, le fil ne doit pas bouger avec, et personne ne pensera à vérifier :
 * c'est le rôle de ces deux fonctions et de l'aller-retour testé dans
 * test/test_usb_mode.c, qui casse dès que l'une des deux numérotations dérive
 * sans l'autre.
 *
 * Les deux sens sont écrits en `switch` sur des NOMS, jamais par un cast ni par
 * une table indexée : un cast ferait exactement ce qu'on veut interdire, et une
 * table se recopie sans que le compilateur ait son mot à dire.
 */

#include <stdbool.h>
#include <stdint.h>

#include "link/link_proto.h"
#include "usb/usb_mode.h"

/*
 * Valeur de fil → mode interne. Rend false, sans toucher `out`, pour toute
 * valeur que le contrat n'attribue pas : le coffre reste alors dans son mode
 * courant (link_proto_mode_request() → LINK_MODE_REQ_REFUSE).
 */
static inline bool usb_mode_from_wire(uint8_t wire, usb_mode_t *out)
{
    usb_mode_t m;

    switch (wire) {
    case LINK_USB_MODE_NONE:    m = USB_MODE_NONE;    break;
    case LINK_USB_MODE_STORAGE: m = USB_MODE_STORAGE; break;
    case LINK_USB_MODE_PGP:     m = USB_MODE_PGP;     break;
    case LINK_USB_MODE_OTP:     m = USB_MODE_OTP;     break;
    case LINK_USB_MODE_FIDO:    m = USB_MODE_FIDO;    break;
    case LINK_USB_MODE_OATH:    m = USB_MODE_OATH;    break;
    default:                    return false;
    }

    if (out != NULL) {
        *out = m;
    }
    return true;
}

/*
 * Mode interne → valeur de fil. Rend LINK_USB_MODE_COUNT — une valeur qu'aucun
 * maître ne doit voir — pour un mode hors bornes, plutôt que de propager
 * l'aberration sur le fil.
 *
 * N'a pas d'appelant dans le firmware aujourd'hui : elle existe pour que
 * l'aller-retour soit testable dans LES DEUX SENS. Une bijection dont on ne
 * vérifie qu'un sens n'est pas une bijection — deux modes internes pourraient
 * tomber sur la même valeur de fil sans qu'aucun test ne le voie.
 */
static inline uint8_t usb_mode_to_wire(usb_mode_t mode)
{
    switch (mode) {
    case USB_MODE_NONE:    return LINK_USB_MODE_NONE;
    case USB_MODE_STORAGE: return LINK_USB_MODE_STORAGE;
    case USB_MODE_PGP:     return LINK_USB_MODE_PGP;
    case USB_MODE_OTP:     return LINK_USB_MODE_OTP;
    case USB_MODE_FIDO:    return LINK_USB_MODE_FIDO;
    case USB_MODE_OATH:    return LINK_USB_MODE_OATH;
    default:               return LINK_USB_MODE_COUNT;
    }
}
