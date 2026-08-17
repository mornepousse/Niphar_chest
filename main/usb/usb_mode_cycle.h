#pragma once

/*
 * usb_mode_cycle — le cycle de modes de la carte-clé.
 *
 * Morceau PUR, sorti pour le test hôte sur le modèle de usb_mode_state.h et
 * de security/ccid_zlp.h : usb_mode.c lui-même ne compile qu'avec ESP-IDF.
 *
 * Trois crans, pgp, otp et fido, et USB_MODE_NONE n'y revient jamais. C'est
 * délibéré : la clé arrive muette au branchement — le principe « rien au
 * demarrage » du projet vaut sur les trois cartes — et le premier appui
 * l'arme. Ensuite, on la fait taire en la debranchant, pas en cherchant le
 * bon nombre d'appuis.
 *
 * USB_MODE_STORAGE n'y figure pas : cette carte n'a pas de microSD.
 */

#include "usb/usb_mode.h"

static inline usb_mode_t usb_mode_cycle_after(usb_mode_t current)
{
    switch (current) {
    case USB_MODE_PGP:  return USB_MODE_OTP;
    case USB_MODE_OTP:  return USB_MODE_FIDO;
    case USB_MODE_FIDO: return USB_MODE_PGP;
    /* NONE, STORAGE et toute valeur aberrante atterrissent sur PGP : le
     * defaut doit etre une valeur connue, pas la propagation de l'aberration. */
    default:            return USB_MODE_PGP;
    }
}
