#pragma once

/*
 * Descripteurs USB du mode PGP — la carte OpenPGP exposée à l'hôte comme un
 * lecteur de carte à puce, classe CCID (USB CCID Rev 1.1), portée depuis
 * KeSp_firmware.
 *
 * Ce module ne connaît que les descripteurs et les chaînes : le protocole
 * CCID et le worker qui dialogue avec openpgp_card.c restent dans
 * security/ccid.c, qui ne dépend pas du mode. C'est usb_mode.c qui relie les
 * deux, en appelant usb_device_install() avec ce que ce fichier expose — sur
 * le modèle exact de usb/mode_storage.h.
 */

#include <stdint.h>

/* Bloc de configuration TinyUSB (TUD_CONFIG_DESCRIPTOR + l'interface CCID)
 * pour le port pleine vitesse. */
const uint8_t *mode_pgp_fs_config(void);

/* Même configuration, vue haute vitesse — voir mode_pgp.c sur pourquoi les
 * deux tableaux sont identiques ici, contrairement à mode_storage. */
const uint8_t *mode_pgp_hs_config(void);

/*
 * Table de chaînes du mode PGP, prête pour usb_device_install(). *out_count
 * reçoit sa taille (ignoré si NULL). Le numéro de série est recopié depuis
 * usb_device_serial() à chaque appel — bon marché, celui-ci met en cache.
 */
const char **mode_pgp_strings(int *out_count);
