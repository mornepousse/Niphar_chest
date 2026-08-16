#pragma once

/*
 * WT9932P4-TINY — la cle de securite autonome.
 *
 * Troisieme carte du projet, et la premiere a avoir un vrai contact. Ce n'est
 * ni un kit ni un brouillon du coffre : c'est l'autre variante de produit — une
 * cle independante, la ou niphar_chest est la meme cle integree au clavier.
 * Elles ne different que par la source de la presence physique.
 *
 * Brochage etabli sur le schema constructeur (WT9932P4-TINY_1v2, JLCEDA,
 * revise 2025-08-07) et recoupe au TRM. Voir docs/HARDWARE.md.
 */

#include "driver/gpio.h"

#define BOARD_NAME "wt9932_key"

/* ------------------------------------------------------------------------- */
/* Axes de carte.                                                             */
/* ------------------------------------------------------------------------- */

#define BOARD_CONFIRM_SOURCE  BOARD_CONFIRM_BUTTON
#define BOARD_LINK_AVAILABLE  0   /* pas de lien SPI : la cle est autonome */
#define BOARD_CONSOLE_ACTIONS 1   /* la console reste utilisable pour le dev */
#define BOARD_HAS_SD          0   /* aucun connecteur microSD au schema */

/* ------------------------------------------------------------------------- */
/* Boutons — deux, un metier chacun.                                          */
/* ------------------------------------------------------------------------- */

/*
 * PAS sur IO35, et c'est le point le plus important de ce fichier.
 *
 * IO35 porte le bouton BOOT du module, et c'est le pin de strapping du mode de
 * boot du P4 — l'equivalent de l'IO0 des ESP32-S3 (« ESP32-P4 has five
 * strapping pins: GPIO34, GPIO35, GPIO36, GPIO37, GPIO38 », TRM ch. 11 p. 795 ;
 * table 11.2-2 : GPIO35 = 0 au reset selectionne un mode download).
 *
 * Le silicium autoriserait pourtant son usage apres le boot (« After the reset
 * is released, the strapping pins work as normal-function pins », §11.2.1). On
 * s'en prive quand meme : un appui pendant la mise sous tension empecherait la
 * cle de demarrer, un reset accidentel bouton enfonce la ferait disparaitre du
 * bus, et le garde-fou 1 de fast.sh — qui exclut deja les en-tetes de carte —
 * resterait vert pendant que le sens de GPIO35 s'y inverserait, de « reserve »
 * a « bouton utilisateur ».
 *
 * IO26 a IO33 sont les seules broches du P4 dont les trois colonnes de la table
 * GPIO sont vides : ni fonction analogique, ni LP GPIO, ni restriction
 * (ESP-IDF Programming Guide, « GPIO & RTC GPIO — ESP32-P4 », § GPIO Summary).
 * Elles sortent sur J7, et sont hors des blocs occupes chez les cartes soeurs —
 * microSD sur 39-48, lien S3 sur 7-11.
 *
 * Deux boutons plutot qu'un a appui long/court : avec un seul, le seuil de
 * duree serait la seule chose separant « je confirme cette signature » de « je
 * desinstalle le CCID pendant que l'hote s'en sert ». Une porte de presence
 * physique doit faire une chose.
 *
 * Cables par l'utilisateur vers la masse ; aucun composant externe, le pull-up
 * interne suffit.
 */
#define BOARD_BTN_MODE        GPIO_NUM_32
#define BOARD_BTN_CONFIRM     GPIO_NUM_33
#define BOARD_BTN_ACTIVE_LOW  1

/* ------------------------------------------------------------------------- */
/* LED adressable.                                                            */
/* ------------------------------------------------------------------------- */

/*
 * WS2812 unique, DIN sur IO51 (schema, bloc LED). Alimentee en 5 V alors que
 * la donnee est en 3,3 V : un WS2812 strict exige VIH >= 0,7 x VDD = 3,5 V, on
 * est dessous. Des couleurs fausses ou du scintillement auraient donc une cause
 * MATERIELLE, pas logicielle — ne pas chercher le bug dans led_state.h.
 *
 * La seconde LED de la carte (R13, 1 kOhm) est un temoin d'alimentation non
 * pilotable.
 */
#define BOARD_LED_WS2812      GPIO_NUM_51
#define BOARD_LED_COUNT       1

/* En dernier : il valide tout ce qui precede. */
#include "board_common.h"
