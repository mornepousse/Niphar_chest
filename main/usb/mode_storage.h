#pragma once

/*
 * Descripteurs USB du mode stockage (MSC) — le disque microSD exposé en
 * blocs bruts à l'hôte.
 *
 * Ce module ne connaît que les descripteurs et les chaînes : les callbacks
 * tud_msc_* qui répondent aux commandes SCSI restent dans msc_disk.c, qui ne
 * dépend pas du mode. C'est usb_mode.c qui relie les deux, en appelant
 * msc_disk_init() puis usb_device_install() avec ce que ce fichier expose.
 */

#include <stdint.h>

/* Bloc de configuration TinyUSB (TUD_CONFIG_DESCRIPTOR + TUD_MSC_DESCRIPTOR)
 * pour le port pleine vitesse. */
const uint8_t *mode_storage_fs_config(void);

/* Même configuration, vue haute vitesse (taille de paquet différente). */
const uint8_t *mode_storage_hs_config(void);

/*
 * Table de chaînes du mode stockage, prête pour usb_device_install().
 * *out_count reçoit sa taille (ignoré si NULL). Le numéro de série est
 * recopié depuis usb_device_serial() à chaque appel — bon marché, celui-ci
 * met en cache.
 */
const char **mode_storage_strings(int *out_count);
