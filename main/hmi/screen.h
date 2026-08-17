#pragma once

/*
 * screen — le pilote SSD1306 et la tache d'affichage.
 *
 * Ne decide rien : quel ecran, quels textes, quelle proportion de barre
 * viennent de hmi/screen_view.h et hmi/screen_anim.h (purs, testes sur
 * l'hote). Ce fichier lit l'instantane publie par hmi.c (hmi_snapshot()),
 * appelle ces en-tetes, et pose les pixels resultants sur le bus I2C — dans
 * une tache separee de la tache IHM, pour ne jamais retarder son
 * echantillonnage des boutons (5 ms) derriere un transfert I2C d'image
 * entiere (25-30 ms).
 *
 * N'existe que sur une carte qui a un ecran — voir boards/wt9932_key/board.h
 * (BOARD_OLED_SCL). No-op complet ailleurs.
 */

#include "esp_err.h"

/*
 * Configure le bus I2C sur BOARD_OLED_SCL/BOARD_OLED_SDA, envoie la sequence
 * d'initialisation du SSD1306, et demarre la tache d'affichage.
 *
 * Meme contrat que hmi_init() : une erreur ici n'est pas fatale, la cle reste
 * utilisable sans ecran (console, boutons, LED). A appeler apres hmi_init() —
 * l'ecran affiche l'etat que hmi.c publie, il n'a aucune raison d'exister
 * avant que cet etat existe.
 */
esp_err_t screen_init(void);
