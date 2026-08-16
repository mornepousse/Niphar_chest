#pragma once

/*
 * Ce que le coffre et le kit de dev ont en commun — voir docs/HARDWARE.md
 * (vérifié à la netlist Niphargus, revue 2026-08-06).
 *
 * Le kit JC-ESP32P4-M3-DEV câble la microSD exactement comme le coffre.
 * Vérifié contre le BSP du vendeur (esp32_p4_function_ev_board.h:71-76) et
 * contre le silicium : « card one (SDMMC_HOST_SLOT_0) signals are multiplexed
 * with GPIO39-GPIO48 … via IO MUX » — ESP32-P4 Series Datasheet v0.7, p. 81.
 *
 * Les deux cartes divergent sur le lien S3↔coffre, et sur lui seul.
 *
 * ORDRE D'INCLUSION : chaque boards/<nom>/board.h déclare d'abord son bloc de
 * lien, PUIS inclut ce fichier — qui vérifie l'ensemble en dernier. L'inverse
 * ne marche pas : les gardes ci-dessous s'exécuteraient avant que la carte ait
 * eu l'occasion de se prononcer.
 */

#include "driver/gpio.h"

/* ------------------------------------------------------------------------- */
/* Axes de carte — trois questions distinctes, trois drapeaux.                */
/* ------------------------------------------------------------------------- */

/*
 * BOARD_LINK_AVAILABLE répondait à deux questions à la fois : « y a-t-il un
 * lien SPI ? » et « la béquille console est-elle permise ? ». La fusion tenait
 * tant qu'il n'y avait que deux cartes. La carte-clé la casse : pas de lien,
 * un bouton, et la console conservée. Séparer n'est pas du zèle — sans ça,
 * elle est inexprimable.
 */
#define BOARD_CONFIRM_NONE    0   /* aucune source réelle de présence */
#define BOARD_CONFIRM_LINK    1   /* le S3, par le lien SPI */
#define BOARD_CONFIRM_BUTTON  2   /* un bouton en façade */

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
 * _Static_assert ci-dessous et par scripts/fast.sh.
 */
#define BOARD_USJ_DM        GPIO_NUM_24
#define BOARD_USJ_DP        GPIO_NUM_25

/*
 * Strap de boot du P4. GPIO35 seul décide entre boot applicatif et mode
 * download (TRM table 11.2-2) ; GPIO34/36/37/38 sont sans effet tant qu'il est
 * haut. Déclaré ici pour que les _Static_assert puissent le refuser.
 */
#define BOARD_BOOT_STRAP    GPIO_NUM_35

#define BOARD_PIN_IS_RESERVED(p) \
    ((p) == BOARD_USJ_DM || (p) == BOARD_USJ_DP || (p) == BOARD_BOOT_STRAP)

/*
 * Chaque carte DOIT se prononcer sur le lien. Sans ce garde, une carte qui
 * oublierait la macro compilerait en silence avec le lien désactivé — le
 * préprocesseur traite une macro inconnue comme 0 dans un #if. Un oubli doit
 * casser le build, pas produire un firmware amputé sans le dire.
 */
#ifndef BOARD_LINK_AVAILABLE
#error "boards/<nom>/board.h doit definir BOARD_LINK_AVAILABLE (0 ou 1)"
#endif
#ifndef BOARD_CONFIRM_SOURCE
#error "boards/<nom>/board.h doit definir BOARD_CONFIRM_SOURCE"
#endif
#ifndef BOARD_CONSOLE_ACTIONS
#error "boards/<nom>/board.h doit definir BOARD_CONSOLE_ACTIONS (0 ou 1)"
#endif
#ifndef BOARD_HAS_SD
#error "boards/<nom>/board.h doit definir BOARD_HAS_SD (0 ou 1)"
#endif

_Static_assert(BOARD_CONFIRM_SOURCE == BOARD_CONFIRM_NONE
            || BOARD_CONFIRM_SOURCE == BOARD_CONFIRM_LINK
            || BOARD_CONFIRM_SOURCE == BOARD_CONFIRM_BUTTON,
               "BOARD_CONFIRM_SOURCE n'a pas une des trois valeurs connues");

#if BOARD_CONFIRM_SOURCE == BOARD_CONFIRM_LINK
_Static_assert(BOARD_LINK_AVAILABLE,
               "presence annoncee par le lien sur une carte qui n'en a pas");
#endif

/* `defined()` n'existe pas dans une expression C : ce contrôle-ci se fait au
 * préprocesseur, pas en _Static_assert. */
#if BOARD_CONFIRM_SOURCE != BOARD_CONFIRM_BUTTON && defined(BOARD_BTN_CONFIRM)
#error "bouton de confirmation declare sur une carte dont ce n'est pas la source"
#endif

_Static_assert(!BOARD_PIN_IS_RESERVED(BOARD_SD_CLK),
               "BOARD_SD_CLK empiete sur un pin reserve (USB-Serial-JTAG ou strap de boot)");
_Static_assert(!BOARD_PIN_IS_RESERVED(BOARD_SD_CMD),
               "BOARD_SD_CMD empiete sur un pin reserve (USB-Serial-JTAG ou strap de boot)");
_Static_assert(!BOARD_PIN_IS_RESERVED(BOARD_SD_D0),
               "BOARD_SD_D0 empiete sur un pin reserve (USB-Serial-JTAG ou strap de boot)");
_Static_assert(!BOARD_PIN_IS_RESERVED(BOARD_SD_D1),
               "BOARD_SD_D1 empiete sur un pin reserve (USB-Serial-JTAG ou strap de boot)");
_Static_assert(!BOARD_PIN_IS_RESERVED(BOARD_SD_D2),
               "BOARD_SD_D2 empiete sur un pin reserve (USB-Serial-JTAG ou strap de boot)");
_Static_assert(!BOARD_PIN_IS_RESERVED(BOARD_SD_D3),
               "BOARD_SD_D3 empiete sur un pin reserve (USB-Serial-JTAG ou strap de boot)");

#if BOARD_LINK_AVAILABLE
_Static_assert(!BOARD_PIN_IS_RESERVED(BOARD_LINK_CS),
               "BOARD_LINK_CS empiete sur un pin reserve");
_Static_assert(!BOARD_PIN_IS_RESERVED(BOARD_LINK_MOSI),
               "BOARD_LINK_MOSI empiete sur un pin reserve");
_Static_assert(!BOARD_PIN_IS_RESERVED(BOARD_LINK_SCK),
               "BOARD_LINK_SCK empiete sur un pin reserve");
_Static_assert(!BOARD_PIN_IS_RESERVED(BOARD_LINK_MISO),
               "BOARD_LINK_MISO empiete sur un pin reserve");
_Static_assert(!BOARD_PIN_IS_RESERVED(BOARD_LINK_IRQ),
               "BOARD_LINK_IRQ empiete sur un pin reserve");
#endif
