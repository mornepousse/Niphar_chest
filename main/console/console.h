#pragma once

/*
 * Console de diagnostic, sur l'USB-Serial-JTAG exclusivement.
 *
 * C'est le dernier recours de diagnostic du coffre, et son unique voie de
 * reflash : rien ne doit pouvoir la tuer. Un échec ailleurs se journalise ici,
 * il ne panique pas.
 */

#include "esp_err.h"

/* Démarre le REPL. Ne rend pas la main tant que la console vit. */
esp_err_t console_start(void);
