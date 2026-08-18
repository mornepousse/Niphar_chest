#pragma once

/*
 * Descripteurs USB du mode OATH — les comptes TOTP exposés à l'hôte comme un
 * lecteur de carte à puce, classe CCID, sur le modèle exact de usb/mode_pgp.h.
 *
 * Deuxième applet sur le MÊME transport. Ce fichier ne connaît que les
 * descripteurs, les chaînes, et le câblage vers security/oath_proto.c : le
 * protocole CCID et le worker qui dialogue avec l'applet restent dans
 * security/ccid.c, qui ne dépend d'aucun mode. C'est usb_mode.c qui relie les
 * deux.
 *
 * Pourquoi un sixième mode plutôt qu'un second applet greffé sur le mode PGP
 * (décision 5 de docs/superpowers/specs/2026-08-18-oath-totp-design.md) : « plein
 * de choses, une à la fois ». Une carte qui répondrait à la fois au SELECT
 * OpenPGP et au SELECT YKOATH exposerait les deux surfaces d'attaque dès qu'on
 * en veut une.
 */

#include <stdint.h>

/* Bloc de configuration TinyUSB (TUD_CONFIG_DESCRIPTOR + l'interface CCID)
 * pour le port pleine vitesse. */
const uint8_t *mode_oath_fs_config(void);

/* Même configuration, vue haute vitesse — les deux tableaux ne diffèrent que
 * par wMaxPacketSize (64 / 512), comme pour mode_pgp. */
const uint8_t *mode_oath_hs_config(void);

/*
 * Table de chaînes du mode OATH, prête pour usb_device_install(). *out_count
 * reçoit sa taille (ignoré si NULL). Le numéro de série est recopié depuis
 * usb_device_serial() à chaque appel.
 */
const char **mode_oath_strings(int *out_count);

/*
 * Charge le magasin, arme le sel du SELECT, et branche l'applet OATH sur le
 * transport CCID. À appeler par usb_mode.c juste APRÈS un
 * usb_device_install() réussi vers USB_MODE_OATH — jamais avant.
 *
 * L'ordre n'est pas décoratif : usb_device_install() fait tourner
 * ccid_drv_init(), qui remet l'aiguillage d'applet sur son défaut (OpenPGP).
 * Poser l'applet plus tôt le ferait écraser. Même raison, à un détail près,
 * que mode_pgp_data_load() — voir son commentaire dans mode_pgp.c.
 *
 * Rejouée à CHAQUE entrée dans le mode, pas seulement à la première : c'est
 * ce qui garantit que le magasin en RAM reflète la NVS après une session où
 * un autre mode a pu écrire (le magasin est partagé avec le mode OTP).
 */
void mode_oath_start(void);

/*
 * Met le worker CCID au repos, débranche l'applet, et efface le contexte —
 * secret en attente compris. À appeler par usb_mode.c juste AVANT
 * usb_device_uninstall() quand on quitte USB_MODE_OATH — jamais après : le
 * worker poste des callbacks sur la file de tud_task, que tud_deinit()
 * détruit. Voir ccid_shutdown() dans security/ccid.h et la divergence
 * BLOQUANT 1 en tête de security/ccid.c.
 */
void mode_oath_stop(void);
