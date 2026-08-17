#pragma once

/*
 * Descripteurs USB du mode OTP — la clé CR-HMAC exposée à l'hôte comme un
 * périphérique HID, sur le modèle du protocole Yubikey OTP (feature reports
 * de 8 octets), portée depuis KeSp_firmware.
 *
 * Ce module ne connaît que les descripteurs, les chaînes, et le report
 * descriptor HID : le protocole OTP lui-même (assemblage des trames,
 * machine à états challenge/réponse) reste dans security/otp_proto.c, et le
 * câblage vers sec_store/sec_confirm/cr_hmac dans security/otp_hid.c — ni
 * l'un ni l'autre ne dépend du mode. C'est usb_mode.c qui relie le tout, en
 * appelant usb_device_install() avec ce que ce fichier expose — sur le
 * modèle exact de usb/mode_pgp.h.
 *
 * Contrairement au CCID, HID est une vraie classe TinyUSB (CFG_TUD_HID=1
 * dans tusb_config.h) : pas de pilote applicatif à force-linker, TinyUSB
 * appelle directement les callbacks tud_hid_* définis dans mode_otp.c.
 */

#include <stdint.h>

/* Bloc de configuration TinyUSB (TUD_CONFIG_DESCRIPTOR + TUD_HID_DESCRIPTOR)
 * pour le port pleine vitesse. */
const uint8_t *mode_otp_fs_config(void);

/* Même configuration, vue haute vitesse — voir mode_otp.c sur pourquoi les
 * deux tableaux sont identiques ici, comme pour mode_pgp. */
const uint8_t *mode_otp_hs_config(void);

/*
 * Table de chaînes du mode OTP, prête pour usb_device_install(). *out_count
 * reçoit sa taille (ignoré si NULL). Le numéro de série est recopié depuis
 * usb_device_serial() à chaque appel — bon marché, celui-ci met en cache.
 */
const char **mode_otp_strings(int *out_count);

/*
 * Pose la table de handlers HID du mode OTP au répartiteur unique (usb/
 * hid_dispatch.h). À appeler par usb_mode.c juste après un
 * usb_device_install() réussi vers le mode OTP — jamais avant. Voir le
 * commentaire en tête de la définition dans mode_otp.c.
 */
void mode_otp_start(void);

/*
 * Retire la table de handlers HID du mode OTP du répartiteur (NULL). À
 * appeler par usb_mode.c juste AVANT usb_device_uninstall() quand on quitte
 * le mode OTP — jamais après. Sur le modèle exact de mode_pgp_stop().
 */
void mode_otp_stop(void);
