#pragma once

/*
 * Coffre Niphar — la carte du produit, dans la moitié gauche du clavier.
 *
 * Pas de bouton reset, pas d'accès matériel au mode download : un firmware qui
 * casse l'USB-Serial-JTAG se répare au fer à souder. C'est cette carte que les
 * garde-fous protègent.
 */

#include "board_common.h"

#define BOARD_NAME "niphar_chest"

/* ------------------------------------------------------------------------- */
/* Lien S3↔coffre — SPI, le coffre en esclave.                                */
/* ------------------------------------------------------------------------- */

#define BOARD_LINK_AVAILABLE 1

/*
 * Quatuor IOMUX natif de SPI2 sur P4 (`spi_slave.rst:157-162`, valeurs
 * esp32p4). Le driver rebascule TOUT le bus sur la matrice GPIO dès qu'un seul
 * signal est hors IOMUX : c'est tout ou rien, et sur un faisceau entre deux
 * cartes la marge de temps de setup sur MISO n'est pas de trop.
 */
#define BOARD_LINK_CS       GPIO_NUM_7
#define BOARD_LINK_MOSI     GPIO_NUM_8
#define BOARD_LINK_SCK      GPIO_NUM_9
#define BOARD_LINK_MISO     GPIO_NUM_10

/*
 * Interruption coffre→clavier. Active à l'état HAUT, avec un pull-down côté S3.
 *
 * Ce n'est pas la convention habituelle, et c'est délibéré : le coffre ne vit
 * qu'en filaire alors que la moitié gauche tourne sur batterie, donc il est non
 * alimenté la plupart du temps et cette broche est alors en haute impédance. Un
 * pull-up injecterait du courant dans un rail éteint à travers les diodes de
 * protection du P4.
 *
 * Côté clavier ce signal arrive sur GPIO46, qui est bien un pin de strapping de
 * l'ESP32-S3 — mais le seul rôle de celui-ci est de contrôler l'impression des
 * messages ROM sur UART0, et avec l'eFuse EFUSE_UART_PRINT_CONTROL à sa valeur
 * par défaut son niveau au reset est explicitement « Ignored » (ESP32-S3 TRM
 * v1.8, table 8.3-1, p. 536). Rien à voir avec GPIO45, qui lui choisit la
 * tension du rail flash.
 *
 * link_spi tient quand même l'invariant « ne jamais asserter avant que le S3
 * ait parlé au moins une fois » : ça ne coûte rien, ça garde la ligne calme
 * pendant le boot du clavier, et si cet eFuse était un jour changé, GPIO46
 * reprendrait un rôle au reset.
 */
#define BOARD_LINK_IRQ      GPIO_NUM_11
#define BOARD_LINK_IRQ_ACTIVE_HIGH 1

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
