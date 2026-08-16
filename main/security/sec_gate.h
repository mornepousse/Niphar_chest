#pragma once

/*
 * Source de l'appui de confirmation.
 *
 * sec_confirm arme et expire ; il ne sait pas d'où vient l'appui. Ce module le
 * lui fournit, et son implémentation dépend de la carte :
 *
 *   niphar_chest  le lien SPI avec le clavier — pas encore écrit, donc toute
 *                 opération expire au bout de SEC_CONFIRM_TIMEOUT_MS. Refuser
 *                 est le comportement honnête.
 *   jc_devkit     une commande console, pour éprouver la pile sur le kit.
 *
 * La variante console n'est PAS une option de configuration : elle est liée à
 * l'absence de lien, donc absente du firmware du coffre par construction. Une
 * confirmation qu'on peut accorder sans geste physique est indistinguable,
 * à l'usage, d'un dispositif qui fonctionne — c'est exactement ce qu'il ne
 * faut pas rendre possible.
 */

#include "esp_err.h"

esp_err_t sec_gate_init(void);

/* Chaîne lisible décrivant d'où viendra l'appui. Jamais NULL. */
const char *sec_gate_source(void);

#include "board.h"
#if !BOARD_LINK_AVAILABLE
/* Béquille de développement — n'existe que sur une carte sans lien. */
void sec_gate_console_confirm(void);
#endif
