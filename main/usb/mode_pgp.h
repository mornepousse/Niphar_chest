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

/*
 * Charge en RAM l'état persistant de la carte OpenPGP (DO, PIN/retry, clés)
 * et y appose le numéro de série dérivé de la MAC. À appeler par usb_mode.c
 * juste après un usb_device_install() réussi vers USB_MODE_PGP — jamais
 * avant, ni au démarrage. Voir le commentaire en tête de la définition dans
 * mode_pgp.c pour le choix et l'exigence d'ordre vis-à-vis de ccid_drv_init().
 */
void mode_pgp_data_load(void);

/*
 * Met le worker CCID au repos. À appeler par usb_mode.c juste AVANT
 * usb_device_uninstall() quand on quitte USB_MODE_PGP — jamais après : le
 * worker poste des callbacks sur la file de tud_task, que tud_deinit()
 * détruit. Voir ccid_shutdown() dans security/ccid.h et la divergence
 * BLOQUANT 1 en tête de security/ccid.c.
 */
void mode_pgp_stop(void);
