#pragma once

/*
 * usb_mode_cycle — le cycle de modes de la carte-clé.
 *
 * Morceau PUR, sorti pour le test hôte sur le modèle de usb_mode_state.h et
 * de security/ccid_zlp.h : usb_mode.c lui-même ne compile qu'avec ESP-IDF.
 *
 * Quatre crans, pgp, otp, fido et oath, et USB_MODE_NONE n'y revient jamais.
 * C'est délibéré : la clé arrive muette au branchement — le principe « rien
 * au demarrage » du projet vaut sur les trois cartes — et le premier appui
 * l'arme. Ensuite, on la fait taire en la debranchant, pas en cherchant le
 * bon nombre d'appuis.
 *
 * OATH est le dernier cran plutot que le voisin de PGP, alors que les deux
 * partagent le transport CCID : un appui de trop depuis PGP tomberait sinon
 * sur un mode qui reexpose un lecteur de carte a puce presque identique,
 * sans que la dalle ne montre grand-chose d'autre qu'un point deplace.
 *
 * USB_MODE_STORAGE n'y figure pas : cette carte n'a pas de microSD.
 */

#include "usb/usb_mode.h"

static inline usb_mode_t usb_mode_cycle_after(usb_mode_t current)
{
    switch (current) {
    case USB_MODE_PGP:  return USB_MODE_OTP;
    case USB_MODE_OTP:  return USB_MODE_FIDO;
    case USB_MODE_FIDO: return USB_MODE_OATH;
    case USB_MODE_OATH: return USB_MODE_PGP;
    /* NONE, STORAGE et toute valeur aberrante atterrissent sur PGP : le
     * defaut doit etre une valeur connue, pas la propagation de l'aberration. */
    default:            return USB_MODE_PGP;
    }
}
