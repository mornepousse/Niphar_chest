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
 *   0x0C-0x0D  CRC16 sur 0x00..0x0B      coffre→S3   (petit-boutiste)
 *   0x0E-0x0F  réservé, à zéro           coffre→S3
 *   0x10       confirmation utilisateur  S3→coffre
 *   0x11-0x13  réservé, à zéro           S3→coffre
 *
 * VINGT octets, cinq mots, et AUCUN mot partagé entre les deux extrémités.
 * C'est la seule chose qui compte dans cette disposition, et elle a coûté une
 * révision : le tampon partagé du `spi_slave_hd` s'écrit par mots de 32 bits
 * côté application. Un champ du coffre logé dans le mot du maître impose donc
 * un lire-modifier-écrire pour republier — et un appui de la propriétaire qui
 * arrive pendant ces quelques cycles est perdu. La carte précédente mettait le
 * CRC du coffre en 0x0E et l'octet du maître en 0x0C : le même mot. Les séparer
 * ferme la fenêtre à la source plutôt que de la réduire.
 *
 * Corrigé pendant que le maître n'existait pas encore — plus tard, il aurait
 * fallu reflasher les deux dépôts ensemble.
 *
 * Le CRC ne couvre QUE les champs écrits par le coffre (LINK_REG_CRC_SPAN) :
 * la plage du maître, l'inclure ferait invalider le bloc par toute écriture
 * légitime du S3.
 */
#define LINK_REG_MAGIC          0x00
#define LINK_REG_VERSION        0x04
#define LINK_REG_STATE          0x05
#define LINK_REG_PENDING_OP     0x06
#define LINK_REG_CONFIRM_COUNT  0x08
#define LINK_REG_CRC            0x0C
#define LINK_REG_RESERVED       0x0E
#define LINK_REG_USER_CONFIRM   0x10
#define LINK_REG_SIZE           0x14

/* Étendue couverte par le CRC : du début jusqu'à la fin du compteur. */
#define LINK_REG_CRC_SPAN       0x0C

/*
 * Les deux plages de propriété, déclarées comme plages et pas comme liste
 * d'offsets : c'est sur elles que porte l'invariant du transport, et une
 * propriété se vérifie, une liste se recopie.
 *
 * Chaque plage couvre un nombre entier de mots et commence sur une frontière de
 * mot : c'est ce qui permet à chaque côté de publier le sien d'un seul bloc.
 */
#define LINK_REG_CHEST_BASE     0x00
#define LINK_REG_CHEST_LEN      0x10
#define LINK_REG_MASTER_BASE    0x10
#define LINK_REG_MASTER_LEN     0x04

/* Bits d'état. */
#define LINK_STATE_SD_PRESENT   (1u << 0)
#define LINK_STATE_USB_MOUNTED  (1u << 1)
/*
 * LINK_STATE_READY — sémantique figée, parce que le maître va s'en servir pour
 * décider s'il peut demander quelque chose : **`app_main()` est allé au bout**.
 * Toutes les initialisations du coffre ont été tentées (microSD, USB, source de
 * confirmation, IHM, écran) et la console tourne.
 *
 * Ce que le bit ne dit PAS, et qu'il ne faut pas lui faire dire : que ces
 * initialisations aient RÉUSSI. Elles ne sont pas fatales une à une, et le
 * coffre démarre volontairement sans rien exposer. L'état réel des sous-systèmes
 * se lit aux autres bits (LINK_STATE_SD_PRESENT, LINK_STATE_USB_MOUNTED).
 *
 * À l'envers, c'est ce qui le rend utile : tant qu'il est à zéro sur un coffre
 * présent, le démarrage est en cours, et une requête du maître tomberait sur des
 * modules à moitié installés.
 */
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
 * N'écrit AUCUN octet de la plage du maître (LINK_REG_MASTER_BASE), qui lui
 * appartient — un appui déjà posé et pas encore lu y survit intact.
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
