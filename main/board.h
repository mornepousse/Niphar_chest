#pragma once

/*
 * Contrat matériel du coffre — voir docs/HARDWARE.md (vérifié à la netlist
 * Niphargus, revue 2026-08-06).
 *
 * Le kit JC-ESP32P4-M3-DEV câble la microSD exactement comme le coffre, d'où
 * une cible de build unique. Vérifié contre le BSP du vendeur
 * (esp32_p4_function_ev_board.h) et contre le silicium : « card one
 * (SDMMC_HOST_SLOT_0) signals are multiplexed with GPIO39-GPIO48 … via IO MUX »
 * — ESP32-P4 Series Datasheet v0.7, p. 81.
 */

#include "driver/gpio.h"

/* ------------------------------------------------------------------------- */
/* microSD — SDMMC_HOST_SLOT_0, pins IOMUX fixes.                             */
/* ------------------------------------------------------------------------- */

#define BOARD_SD_CLK        GPIO_NUM_43
#define BOARD_SD_CMD        GPIO_NUM_44
#define BOARD_SD_D0         GPIO_NUM_39
#define BOARD_SD_D1         GPIO_NUM_40
#define BOARD_SD_D2         GPIO_NUM_41
#define BOARD_SD_D3         GPIO_NUM_42

#define BOARD_SD_BUS_WIDTH  4

/*
 * Pas de card-detect ni de write-protect : le connecteur Würth 693072010801 du
 * coffre n'a pas de contact de détection, et le BSP du kit déclare également
 * SDMMC_SLOT_NO_CD / SDMMC_SLOT_NO_WP. La présence se constate en interrogeant
 * la carte, pas en lisant un GPIO.
 */
#define BOARD_SD_HAS_CARD_DETECT   0

/*
 * IO fixe 3,3 V, pas de commutation 1,8 V : plafond High-Speed, pas de SDR104.
 * La fréquence elle-même est une politique du pilote, pas du câblage : elle vit
 * dans storage/sd_card.c. Ce fichier ne décrit que le brochage.
 */

/* ------------------------------------------------------------------------- */
/* Intouchables.                                                              */
/* ------------------------------------------------------------------------- */

/*
 * USB-Serial-JTAG. C'est le SEUL chemin de flash et de debug du coffre : pas de
 * bouton reset, pas d'accès matériel au mode download. Réaffecter ces GPIO, ou
 * publier un firmware qui empêche l'énumération, transforme la carte en objet à
 * dessouder.
 *
 * Le kit de dev, lui, pardonne (CH340C + bouton BOOTMODE). Cette règle ne se
 * vérifiera donc jamais à l'exécution sur le kit : elle tient par les
 * _Static_assert ci-dessous et par scripts/check.sh.
 */
#define BOARD_USJ_DM        GPIO_NUM_24
#define BOARD_USJ_DP        GPIO_NUM_25

#define BOARD_PIN_IS_RESERVED(p) ((p) == BOARD_USJ_DM || (p) == BOARD_USJ_DP)

_Static_assert(!BOARD_PIN_IS_RESERVED(BOARD_SD_CLK),
               "BOARD_SD_CLK empiete sur l'USB-Serial-JTAG (GPIO24/25) : le coffre deviendrait irrecuperable");
_Static_assert(!BOARD_PIN_IS_RESERVED(BOARD_SD_CMD),
               "BOARD_SD_CMD empiete sur l'USB-Serial-JTAG (GPIO24/25) : le coffre deviendrait irrecuperable");
_Static_assert(!BOARD_PIN_IS_RESERVED(BOARD_SD_D0),
               "BOARD_SD_D0 empiete sur l'USB-Serial-JTAG (GPIO24/25) : le coffre deviendrait irrecuperable");
_Static_assert(!BOARD_PIN_IS_RESERVED(BOARD_SD_D1),
               "BOARD_SD_D1 empiete sur l'USB-Serial-JTAG (GPIO24/25) : le coffre deviendrait irrecuperable");
_Static_assert(!BOARD_PIN_IS_RESERVED(BOARD_SD_D2),
               "BOARD_SD_D2 empiete sur l'USB-Serial-JTAG (GPIO24/25) : le coffre deviendrait irrecuperable");
_Static_assert(!BOARD_PIN_IS_RESERVED(BOARD_SD_D3),
               "BOARD_SD_D3 empiete sur l'USB-Serial-JTAG (GPIO24/25) : le coffre deviendrait irrecuperable");
