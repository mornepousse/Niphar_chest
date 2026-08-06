#pragma once

/*
 * Périphérique USB — port haute vitesse (OTG 2.0, PHY UTMI).
 *
 * C'est le chemin « produit » : celui par lequel l'hôte voit un disque. Il est
 * physiquement distinct de l'USB-Serial-JTAG, qui reste la voie de flash et de
 * debug (port 3 du hub sur le coffre, cf. docs/HARDWARE.md). Les deux doivent
 * pouvoir vivre en même temps.
 */

#include <stdbool.h>

#include "esp_err.h"

/* Installe le pilote et démarre l'énumération. */
esp_err_t usb_device_start(void);

/* Vrai si l'hôte a configuré le périphérique. */
bool usb_device_mounted(void);
