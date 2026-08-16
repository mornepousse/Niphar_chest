/*
 * usb_mode_name vit seule dans ce fichier, qui n'inclut que usb_mode.h : c'est
 * la seule partie de usb_mode qui soit de la logique pure, compilable sur
 * l'hôte pour les tests. Tout ce qui touche au matériel reste dans
 * usb_mode.c.
 */
#include "usb/usb_mode.h"

const char *usb_mode_name(usb_mode_t mode)
{
    switch (mode) {
    case USB_MODE_NONE:    return "aucun";
    case USB_MODE_STORAGE: return "stockage";
    case USB_MODE_PGP:     return "carte OpenPGP";
    case USB_MODE_OTP:     return "clé CR-HMAC";
    default:               return "inconnu";
    }
}
