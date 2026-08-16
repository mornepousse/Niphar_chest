#pragma once
#include <stdint.h>

/*
 * CRC-16/X-25 (polynôme 0x8408 réfléchi, init 0xFFFF).
 *
 * Repris verbatim de KeSp_firmware/main/security/cr_crc16.c. Ne pas en écrire
 * une seconde variante : la pile CCID qu'on portera plus tard s'en sert aussi,
 * et deux CRC divergents entre les deux dépôts seraient une source de bugs
 * silencieux à l'interface.
 */
/* Déplacé de main/link/ vers main/sys/ le 2026-08-07 : la pile CCID portée
 * depuis KeSp s'en sert aussi, et deux copies divergentes entre volets
 * seraient exactement le bug silencieux que ce fichier prétend éviter. */
uint16_t cr_crc16(const uint8_t *data, uint16_t len);
