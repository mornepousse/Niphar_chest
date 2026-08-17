#pragma once

/*
 * Descripteurs USB du mode FIDO — la carte-clé exposée à l'hôte comme un
 * authentificateur U2F/CTAP-HID (page d'usage FIDO Alliance 0xF1D0, usage
 * 0x01), rapport 64 octets bidirectionnel (INOUT), sur le modèle de
 * usb/mode_otp.h pour la partie descripteurs/chaînes.
 *
 * Ce module ne connaît que les descripteurs, les chaînes, et le report
 * descriptor HID, plus le câblage vers security/ctaphid.c (réassemblage des
 * trames) : le protocole CTAP proprement dit (INIT, PING pour l'instant ;
 * MSG et CBOR viendront avec les tâches suivantes du plan) reste dans
 * mode_fido.c mais n'en dépend d'aucune autre partie du firmware — c'est le
 * répartiteur HID (usb/hid_dispatch.h) qui relie ce fichier à TinyUSB.
 */

#include <stdint.h>

/* Bloc de configuration TinyUSB (TUD_CONFIG_DESCRIPTOR + interface HID
 * INOUT) pour le port pleine vitesse. */
const uint8_t *mode_fido_fs_config(void);

/* Même configuration, vue haute vitesse — deux tableaux identiques, comme
 * pour mode_otp (CFG_TUD_HID_EP_BUFSIZE ne dépend pas de la vitesse). */
const uint8_t *mode_fido_hs_config(void);

/*
 * Table de chaînes du mode FIDO, prête pour usb_device_install().
 * *out_count reçoit sa taille (ignoré si NULL). Le numéro de série est
 * recopié depuis usb_device_serial() à chaque appel.
 */
const char **mode_fido_strings(int *out_count);

/*
 * Pose la table de handlers HID du mode FIDO au répartiteur unique (usb/
 * hid_dispatch.h). À appeler par usb_mode.c juste après un
 * usb_device_install() réussi vers USB_MODE_FIDO — jamais avant. Sur le
 * modèle exact de mode_otp_start().
 */
void mode_fido_start(void);

/*
 * Retire la table de handlers HID du mode FIDO du répartiteur (NULL). À
 * appeler par usb_mode.c juste AVANT usb_device_uninstall() quand on quitte
 * USB_MODE_FIDO — jamais après. Sur le modèle exact de mode_otp_stop().
 */
void mode_fido_stop(void);
