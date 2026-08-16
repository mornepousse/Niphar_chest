#pragma once

/*
 * Source de l'appui de confirmation.
 *
 * sec_confirm arme et expire ; il ne sait pas d'où vient l'appui. Ce module le
 * lui fournit, et son implémentation dépend de BOARD_CONFIRM_SOURCE, un axe
 * distinct de BOARD_CONSOLE_ACTIONS — les trois cartes les font diverger :
 *
 *   niphar_chest  BOARD_CONFIRM_LINK,   BOARD_CONSOLE_ACTIONS=0 — le lien SPI
 *                 avec le clavier, pas encore écrit, donc toute opération
 *                 expire au bout de SEC_CONFIRM_TIMEOUT_MS. Refuser est le
 *                 comportement honnête.
 *   jc_devkit     BOARD_CONFIRM_NONE,   BOARD_CONSOLE_ACTIONS=1 — une commande
 *                 console, pour éprouver la pile sur le kit.
 *   wt9932_key    BOARD_CONFIRM_BUTTON, BOARD_CONSOLE_ACTIONS=1 — un bouton en
 *                 façade, ET la console reste utilisable pour le dev : les
 *                 deux axes ne coïncident plus ici.
 *
 * BOARD_CONSOLE_ACTIONS ne conditionne QUE sec_gate_console_confirm (la
 * béquille de développement) : sa présence ne dit rien de la source réelle de
 * confirmation, qui est BOARD_CONFIRM_SOURCE. Une confirmation qu'on peut
 * accorder sans geste physique est indistinguable, à l'usage, d'un dispositif
 * qui fonctionne — c'est exactement ce que la béquille ne doit jamais devenir
 * sur une carte qui a une source réelle (lien ou bouton).
 */

#include "esp_err.h"

esp_err_t sec_gate_init(void);

/* Chaîne lisible décrivant d'où viendra l'appui. Jamais NULL. */
const char *sec_gate_source(void);

#include "board.h"
#if BOARD_CONSOLE_ACTIONS
/* Béquille de développement — n'existe que sur une carte qui autorise la
 * console à agir. */
void sec_gate_console_confirm(void);
#endif

#if BOARD_CONFIRM_SOURCE == BOARD_CONFIRM_BUTTON
/*
 * Relais du bouton de confirmation. Contrairement a sec_gate_console_confirm,
 * ce n'est PAS une bequille : c'est un geste physique qu'un hote ne peut pas
 * fabriquer. Appele par hmi.c, sur front d'appui stable.
 */
void sec_gate_button_confirm(void);
#endif
