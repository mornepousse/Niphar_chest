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
 * confirmation, qui est BOARD_CONFIRM_SOURCE.
 *
 * La vraie règle n'est PAS « jamais aux côtés d'une source réelle » — ça se
 * lirait dans le code, et ce serait faux : wt9932_key a BOARD_CONFIRM_BUTTON
 * ET BOARD_CONSOLE_ACTIONS=1 en même temps. La règle est plus étroite :
 * interdite sur les cartes à LIEN (famille coffre — voir le
 * _Static_assert dans board_common.h, qui la rend impossible à violer par
 * oubli), permise sur une carte à bouton comme compromis déclaré. Sur la
 * carte-clé, la console reste utilisable pour le dev parce que c'est une clé
 * de développement personnelle, pas un produit scellé — et parce que
 * docs/HARDWARE.md documente déjà, indépendamment de ce compromis, que rien
 * n'isole physiquement son USB-Serial-JTAG de l'hôte (pas de jumpers JP1/JP2
 * comme sur le coffre de production) : quiconque peut y parler à la console
 * peut de toute façon dumper la flash. Ce n'est donc pas cette béquille qui
 * fait la différence de posture de sécurité sur cette carte-là.
 *
 * Sur le coffre, en revanche, une confirmation qu'on peut accorder sans
 * geste physique serait indistinguable, à l'usage, d'un dispositif qui
 * fonctionne — c'est exactement ce que la béquille ne doit jamais devenir là
 * où la présence est censée venir du lien.
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
