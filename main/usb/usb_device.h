#pragma once

/*
 * Périphérique USB — port haute vitesse (OTG 2.0, PHY UTMI).
 *
 * C'est le chemin « produit » : celui par lequel l'hôte voit une fonction du
 * coffre. Il est physiquement distinct de l'USB-Serial-JTAG, qui reste la
 * voie de flash et de debug (port 3 du hub sur le coffre, cf.
 * docs/HARDWARE.md). Les deux doivent pouvoir vivre en même temps.
 *
 * Ce module ne connaît aucun mode : il installe les descripteurs qu'on lui
 * donne, et les retire sur demande. C'est usb_mode.c qui décide quoi lui
 * passer — voir usb/usb_mode.h et usb/mode_storage.h.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Installe le PHY, la tâche TinyUSB et les descripteurs fournis, puis
 * démarre l'énumération. `fs_cfg`/`hs_cfg` sont des blocs de configuration
 * TinyUSB complets (TUD_CONFIG_DESCRIPTOR + descripteurs de classe) pour le
 * port pleine vitesse et haute vitesse respectivement. `strings` est indexée
 * comme les descripteurs de chaîne le veulent : index 0 le LANGID (2 octets
 * bruts, pas une chaîne C), 1 le fabricant, 2 le produit, 3 le numéro de
 * série, et le reste les chaînes propres au mode installé.
 *
 * Échoue avec ESP_ERR_INVALID_STATE si un mode est déjà installé — appeler
 * usb_device_uninstall() d'abord.
 */
esp_err_t usb_device_install(const uint8_t *fs_cfg, const uint8_t *hs_cfg,
                              const char **strings, int string_count);

/*
 * Détache explicitement du bus (l'hôte voit une vraie déconnexion), arrête
 * la tâche qui appelle tud_task(), désinitialise le contrôleur puis libère
 * le PHY. Sûr à appeler même si rien n'est installé — no-op idempotent,
 * pour que usb_mode_init() n'ait pas à savoir si c'est le premier appel.
 */
esp_err_t usb_device_uninstall(void);

/* Vrai si l'hôte a configuré le périphérique. */
bool usb_device_mounted(void);

/*
 * Numéro de série dérivé de la MAC, calculé une seule fois puis mis en
 * cache. C'est une identité de l'appareil, pas d'un mode : chaque table de
 * chaînes de mode_storage.c (et, plus tard, des modes PGP/OTP) l'utilise
 * pour remplir son propre index STRID_SERIAL.
 */
const char *usb_device_serial(void);
