#pragma once

/*
 * JC-ESP32P4-M3-DEV — le kit de dev, seul matériel existant à ce jour.
 *
 * microSD et USB câblés exactement comme le coffre, d'où tout board_common.h.
 * Ce qui diverge, et c'est le seul point : le lien S3↔coffre n'y est pas
 * câblable.
 */

#include "driver/gpio.h"

#define BOARD_NAME "jc_devkit"

/* ------------------------------------------------------------------------- */
/* Lien S3↔coffre — indisponible ici.                                         */
/* ------------------------------------------------------------------------- */

/*
 * Le quatuor SPI du coffre (GPIO7-10) et sa ligne d'interruption (GPIO11) sont
 * déjà pris sur ce kit — I2C du codec ES8311 sur 7/8, I2S sur 9/10, commande de
 * l'ampli sur 11 (BSP du fabricant, esp32_p4_function_ev_board.h:46-55).
 *
 * Les réutiliser serait pire qu'inutile : GPIO7/8 portent un bus I2C avec
 * pull-ups et esclaves vivants, et un esclave I2C tire SDA pendant son ACK. Le
 * trafic SPI finirait par produire un START suivi d'une adresse qui correspond,
 * et l'esclave tirerait la ligne CS en pleine transaction — des erreurs
 * intermittentes inexplicables sur le lien qu'on cherche justement à valider.
 *
 * Le lien est donc compilé hors du firmware sur cette carte. Sa logique reste
 * couverte par les tests hôte (test/test_link_proto.c), qui n'ont besoin
 * d'aucun matériel.
 */
#define BOARD_LINK_AVAILABLE 0

/*
 * Pas de contact sur ce kit : la seule confirmation possible vient de la
 * console, et elle n'a aucune valeur de sécurité — voir sec_gate.h.
 */
#define BOARD_CONFIRM_SOURCE  BOARD_CONFIRM_NONE
#define BOARD_CONSOLE_ACTIONS 1
#define BOARD_HAS_SD          1

/* En dernier : il valide tout ce qui précède. */
#include "board_common.h"
