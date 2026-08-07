#pragma once

/*
 * Protocole du lien S3↔coffre — logique pure.
 *
 * Aucun appel ESP-IDF ici : ce fichier compile sur l'hôte, et c'est délibéré.
 * Le lien est le premier morceau du projet écrit sans pouvoir être prouvé au
 * banc (sur le kit de dev, GPIO7-11 sont pris par le codec audio), donc tout ce
 * qui peut être faux sans être visible vit ici, sous tests.
 *
 * Conception : docs/superpowers/specs/2026-08-07-lien-s3-coffre-design.md
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Version du protocole, présente dans les registres ET dans chaque trame : les
 * deux dépôts ne seront pas toujours flashés ensemble. */
#define LINK_PROTO_VERSION  1

/* Carte des registres partagés, lus et écrits par le maître sans coopération du
 * firmware du coffre (c'est le matériel qui répond).
 *
 *   0x00-0x03  mot magique « NIPH »      coffre→S3
 *   0x04       version du protocole      coffre→S3
 *   0x05       bits d'état               coffre→S3
 *   0x06-0x07  opération en attente      coffre→S3   (petit-boutiste)
 *   0x08-0x0B  confirmations consommées  coffre→S3   (petit-boutiste)
 *   0x0C       confirmation utilisateur  S3→coffre
 *   0x0D       réservé, à zéro
 *   0x0E-0x0F  CRC16 sur 0x00..0x0B      coffre→S3   (petit-boutiste)
 *
 * Le CRC ne couvre QUE les champs écrits par le coffre : l'octet de
 * confirmation appartient au maître, l'inclure ferait invalider le bloc par
 * toute écriture légitime.
 */
#define LINK_REG_MAGIC          0x00
#define LINK_REG_VERSION        0x04
#define LINK_REG_STATE          0x05
#define LINK_REG_PENDING_OP     0x06
#define LINK_REG_CONFIRM_COUNT  0x08
#define LINK_REG_USER_CONFIRM   0x0C
#define LINK_REG_RESERVED       0x0D
#define LINK_REG_CRC            0x0E
#define LINK_REG_SIZE           0x10

/* Étendue couverte par le CRC : du début jusqu'à la fin du compteur. */
#define LINK_REG_CRC_SPAN       0x0C

/* Bits d'état. */
#define LINK_STATE_SD_PRESENT   (1u << 0)
#define LINK_STATE_USB_MOUNTED  (1u << 1)
#define LINK_STATE_READY        (1u << 2)

/* Valeur que le S3 écrit pour signaler un appui réel. Une valeur choisie plutôt
 * que 1 : du bruit sur le bus a peu de chances de la produire. */
#define LINK_USER_CONFIRM_MAGIC 0x5A

typedef struct {
    uint8_t  version;
    uint8_t  state;
    uint16_t pending_op;
    uint32_t confirm_count;
} link_status_t;

/*
 * Sérialise l'état du coffre dans `regs` (LINK_REG_SIZE octets), CRC compris.
 * N'écrit pas l'octet de confirmation, qui appartient au maître.
 */
void link_proto_pack_status(uint8_t *regs, const link_status_t *st);

/*
 * Décode un bloc de registres. Renvoie false — et ne touche pas `out` — si le
 * bloc est plus court que LINK_REG_SIZE, si le mot magique, la version ou le
 * CRC ne conviennent pas, ou si le bloc est celui d'un coffre absent.
 *
 * `len` n'est pas décoratif : le jour où `regs` sera rempli par une transaction
 * SPI, un transfert court ferait lire au-delà du tampon. Le paramètre existe
 * avant le premier appelant, pas après.
 */
bool link_proto_parse_status(const uint8_t *regs, size_t len, link_status_t *out);

/*
 * Vrai si le bloc est celui d'une ligne flottante : uniformément 0x00 ou
 * uniformément 0xFF, selon la terminaison. Coffre absent est l'état ORDINAIRE —
 * le clavier vit sur batterie, le coffre ne s'éveille qu'en filaire.
 */
bool link_proto_is_absent(const uint8_t *regs, size_t len);
