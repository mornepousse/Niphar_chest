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
 * Contrat publié à KeSp : docs/LINK_CONTRACT.md
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Version du protocole, présente dans les registres ET dans chaque trame : les
 * deux dépôts ne seront pas toujours flashés ensemble.
 *
 * 2 depuis le 2026-09-29. Deux défauts de la v1, dont un bloquant :
 *
 *   - le coffre était INERTE. Le protocole portait l'état, l'opération armée,
 *     le compteur et la confirmation — aucun champ pour DEMANDER un mode USB.
 *     Or boards/niphar_chest/board.h pose BOARD_CONSOLE_ACTIONS 0 : sa console
 *     n'a aucun pouvoir. Le coffre démarrait donc en USB_MODE_NONE et RIEN ne
 *     pouvait l'en sortir — microSD qui répond, applets présents, lien qui
 *     fonctionne, et l'hôte qui ne voit jamais rien. La v1 avait traité la
 *     PRÉSENCE (« la présence vient du clavier ») ; personne n'avait porté la
 *     SÉLECTION.
 *   - la reprise de confirmation se faisait par code d'opération, pas par
 *     instance. Relevé par l'équipe KeSp : si la première écriture se perd et
 *     qu'une opération du MÊME code est armée entre-temps, la reprise la
 *     confirme. La propriétaire appuie pour « CODE OTP GITHUB », l'écriture se
 *     perd, l'opération expire, l'hôte en arme une pour « CODE OTP BANQUE », la
 *     reprise la confirme. Elle n'a jamais donné son accord pour ce compte —
 *     et l'écran, qui existe précisément pour que son appui veuille dire
 *     quelque chose, lui montrait l'autre.
 */
#define LINK_PROTO_VERSION  2

/* Carte des registres partagés, lus et écrits par le maître sans coopération du
 * firmware du coffre (c'est le matériel qui répond).
 *
 *   0x00-0x03  mot magique « NIPH »      coffre→S3
 *   0x04       version du protocole      coffre→S3
 *   0x05       bits d'état               coffre→S3
 *   0x06-0x07  opération en attente      coffre→S3   (petit-boutiste)
 *   0x08-0x0B  confirmations consommées  coffre→S3   (petit-boutiste)
 *   0x0C       numéro d'instance         coffre→S3
 *   0x0D       mode USB ACTIF            coffre→S3
 *   0x0E-0x0F  CRC16 sur 0x00..0x0D      coffre→S3   (petit-boutiste)
 *   0x10       confirmation utilisateur  S3→coffre
 *   0x11       écho du numéro d'instance S3→coffre
 *   0x12       mode USB demandé          S3→coffre
 *   0x13       réservé                   S3→coffre
 *
 * VINGT octets, cinq mots, et AUCUN mot partagé entre les deux extrémités.
 * C'est la seule chose qui compte dans cette disposition, et elle a coûté une
 * révision : le tampon partagé du `spi_slave_hd` s'écrit par mots de 32 bits
 * côté application. Un champ du coffre logé dans le mot du maître impose donc
 * un lire-modifier-écrire pour republier — et un appui de la propriétaire qui
 * arrive pendant ces quelques cycles est perdu.
 *
 * LE CRC EST REVENU EN 0x0E, ET CE N'EST PAS UN RETOUR EN ARRIÈRE. La carte
 * d'avant la séparation des mots le mettait bien là — mais l'octet du maître
 * était alors en 0x0C, donc dans LE MÊME MOT que lui. C'était ça, le défaut,
 * pas l'offset. Le maître vit désormais en 0x10-0x13 et le coffre possède
 * 0x00-0x0F en entier : aucun mot n'est partagé, l'argument tient toujours, et
 * le CRC peut couvrir tout ce qui le précède de façon CONTIGUË.
 *
 * Deux autres placements du numéro d'instance ont été écartés, et la raison
 * appartient au contrat autant qu'au code :
 *   - instance en 0x0E, CRC laissé en 0x0C-0x0D : l'étendue couverte devenait
 *     DISCONTINUE (0x00..0x0B puis 0x0E). Les deux implémentations devraient
 *     reproduire exactement le même saut, et rien ne rattraperait celle qui se
 *     tromperait — le bloc serait simplement toujours refusé, sans explication.
 *   - instance hors du CRC : un bit retourné dessus ne casse rien de grave,
 *     mais produit un refus de confirmation que ni le coffre ni le maître ne
 *     saurait expliquer. Le CRC existe pour que « c'est corrompu » soit une
 *     réponse disponible.
 *
 * Le CRC ne couvre QUE les champs écrits par le coffre (LINK_REG_CRC_SPAN) :
 * l'étendre à la plage du maître ferait invalider le bloc par toute écriture
 * légitime du S3.
 */
#define LINK_REG_MAGIC          0x00
#define LINK_REG_VERSION        0x04
#define LINK_REG_STATE          0x05
#define LINK_REG_PENDING_OP     0x06
#define LINK_REG_CONFIRM_COUNT  0x08
#define LINK_REG_INSTANCE       0x0C
#define LINK_REG_USB_MODE_ACTIVE 0x0D
#define LINK_REG_CRC            0x0E
#define LINK_REG_USER_CONFIRM   0x10
#define LINK_REG_CONFIRM_ECHO   0x11
#define LINK_REG_USB_MODE_REQ   0x12
#define LINK_REG_SIZE           0x14

/* Étendue couverte par le CRC : du début jusqu'à l'octet qui le précède. */
#define LINK_REG_CRC_SPAN       0x0E

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

/*
 * Valeurs DE FIL du mode USB demandé (0x12), figées par ce contrat et
 * INDÉPENDANTES de l'énumération interne usb_mode_t.
 *
 * Deux numérotations et pas une, alors qu'elles coïncident aujourd'hui : un
 * jour quelqu'un réordonnera usb_mode_t — pour insérer un mode entre deux
 * autres, pour grouper les personnalités CCID — et le fil ne doit pas bouger
 * avec. La traduction vit dans usb/usb_mode_wire.h, avec un aller-retour testé
 * qui casse si l'une des deux dérive sans l'autre.
 *
 * Une valeur hors de cette liste est REFUSÉE : le coffre reste dans son mode
 * courant et le journalise. Jamais « on prend le plus proche », jamais « on
 * retombe sur aucun » — un mode qu'on n'a pas demandé est aussi faux qu'un
 * mode qu'on n'a pas compris.
 */
#define LINK_USB_MODE_NONE      0x00
#define LINK_USB_MODE_STORAGE   0x01
#define LINK_USB_MODE_PGP       0x02
#define LINK_USB_MODE_OTP       0x03
#define LINK_USB_MODE_FIDO      0x04
#define LINK_USB_MODE_OATH      0x05
/* Première valeur non attribuée. Les valeurs de fil sont contiguës depuis 0 :
 * c'est ce qui permet d'écrire le test de validité comme une comparaison, et
 * ça fait partie du contrat au même titre que les six valeurs elles-mêmes. */
#define LINK_USB_MODE_COUNT     0x06

/*
 * Mode actif INDÉTERMINÉ (0x0D uniquement, jamais 0x12).
 *
 * Publié pendant une bascule, c'est-à-dire tant que usb_mode_is_known() est
 * faux. La règle vient de l'octet d'à côté : LINK_STATE_USB_MOUNTED refuse déjà
 * délibérément de compter un mode incertain, parce que « ce que voit l'hôte
 * n'est alors plus garanti, et l'annoncer au clavier serait mentir ». Publier
 * le mode courant pendant une bascule serait ce mensonge-là, pendant la seule
 * seconde où il compte.
 *
 * Hors de la plage contiguë des modes, donc jamais confondu avec l'un d'eux :
 * un maître qui compare 0x0D à ce qu'il a demandé ne peut pas tomber sur une
 * égalité accidentelle en cours de route.
 */
#define LINK_USB_MODE_UNKNOWN   0xFF

typedef struct {
    uint8_t  version;
    uint8_t  state;
    uint16_t pending_op;
    uint32_t confirm_count;
    /*
     * Numéro d'instance de l'opération armée — pas son CODE.
     *
     * C'est la différence entre la v1 et la v2, et elle est tout entière là :
     * deux opérations successives du même code (deux « CODE OTP », pour deux
     * comptes différents) portent des instances DIFFÉRENTES. Le maître renvoie
     * celle qu'il a lue quand il a montré l'opération à la propriétaire ; le
     * coffre n'accepte que celle qu'il a armée. Un appui ne peut donc plus
     * glisser d'une opération à la suivante.
     *
     * Incrémenté à CHAQUE armement (sec_confirm_peek_armed(), octet de poids
     * faible du compteur d'armements) et non à chaque changement de code : deux
     * armements du même code dans le même tour de boucle du lien seraient
     * indiscernables autrement, et c'est exactement le scénario de KeSp.
     *
     * UN OCTET, ET CE QUE « PAS RÉUTILISÉ DANS UNE FENÊTRE RAISONNABLE » VEUT
     * DIRE ICI. La fenêtre qui compte est celle pendant laquelle un écho peut
     * encore arriver : le maître lit le bloc, montre l'opération, attend
     * l'appui, écrit. Elle est bornée par SEC_CONFIRM_TIMEOUT_MS (15 s) —
     * au-delà, sec_confirm a déjà expiré l'opération et l'écho ne vaut plus
     * rien même s'il correspond. Pour qu'une instance se réutilise DANS cette
     * fenêtre, il faut 256 armements en 15 secondes, soit dix-sept par seconde,
     * chacun écrasant le précédent sous les yeux de la propriétaire. C'est une
     * limite déclarée, pas une preuve : un hôte qui ferait ça réaligerait
     * l'instance — mais il devrait pour cela faire défiler 256 opérations sur
     * l'écran pendant qu'elle appuie, là où le défaut de la v1 se déclenchait
     * avec DEUX. L'octet est ce que la disposition donne (elle est déjà partie
     * chez KeSp) ; ce commentaire dit ce qu'il achète.
     */
    uint8_t  instance;
    /*
     * Mode USB RÉELLEMENT INSTALLÉ, en valeur de fil — pas celui qui a été
     * demandé en 0x12.
     *
     * Les deux ne divergent que quand quelque chose ne va pas : une bascule
     * refusée, échouée, ou encore en cours. C'est exactement là que le maître a
     * besoin de la différence, et c'est ce qui manquait à la carte de la v2
     * telle qu'elle est partie chez KeSp : LINK_STATE_USB_MOUNTED dit « quelque
     * chose est monté », jamais QUOI. Le clavier ne pouvait donc afficher que sa
     * propre demande, et un coffre bloqué en none restait silencieux.
     *
     * Vaut LINK_USB_MODE_UNKNOWN tant que le mode est incertain.
     */
    uint8_t  usb_mode_active;
} link_status_t;

/*
 * Ce que le maître a écrit dans SA plage (0x10-0x13), décodé tel quel.
 *
 * Les trois octets sont rendus BRUTS, sans jugement : c'est le rôle des deux
 * prédicats plus bas de statuer, et les séparer permet de les tester
 * indépendamment du transport. Aucun contrôle d'intégrité ne protège cette
 * plage — voir la section 5 du contrat pour ce qui en tient lieu.
 */
typedef struct {
    uint8_t confirm;   /* 0x10 — LINK_USER_CONFIRM_MAGIC pour un appui réel */
    uint8_t echo;      /* 0x11 — instance que le maître a lue et renvoie */
    uint8_t usb_mode;  /* 0x12 — valeur de fil du mode demandé */
} link_master_t;

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
 * Décode la plage du maître. Renvoie false — et ne touche pas `out` — si `len`
 * ne couvre pas tout le bloc.
 *
 * Volontairement SANS contrôle d'intégrité et sans rejet de valeur : ce qui se
 * refuse se refuse plus loin, avec l'instance armée sous la main.
 */
bool link_proto_parse_master(const uint8_t *regs, size_t len, link_master_t *out);

/*
 * L'écriture du maître vaut-elle confirmation pour l'instance `armed` ?
 *
 * Vrai SI ET SEULEMENT SI 0x10 vaut LINK_USER_CONFIRM_MAGIC ET 0x11 vaut
 * `armed`. Les deux, jamais l'un sans l'autre : c'est toute la correction de
 * la v2, et l'écrire comme un prédicat pur est ce qui permet de l'éprouver sur
 * l'hôte, là où le transport ne s'éprouve nulle part.
 *
 * Faux se lit « ignorer en silence » : le coffre ne relaie rien, le compteur de
 * confirmations ne bouge pas, et c'est ce compteur immobile qui dit au maître
 * de relire le bloc et de réessayer avec l'instance courante.
 */
bool link_proto_confirm_accepted(const link_master_t *m, uint8_t armed);

/* Vrai si `wire` est l'une des valeurs de mode que ce contrat attribue. */
bool link_proto_usb_mode_is_known(uint8_t wire);

/*
 * Ce qu'il faut faire de l'octet de mode lu en 0x12.
 *
 * `applied` est la dernière valeur de fil RÉELLEMENT APPLIQUÉE, initialisée à
 * LINK_USB_MODE_NONE au démarrage du coffre — pas la dernière valeur LUE. La
 * distinction est ce qui rend la sélection auto-réparante : après un reboot du
 * coffre, le maître relit 0x12 (remis à zéro avec tout le tampon partagé), voit
 * qu'il diffère du mode qu'il veut, le réécrit, et le coffre voit un changement
 * 0 → N et applique. Un maître qui relit et réécrit à chaque cycle remet donc
 * le coffre d'aplomb sans rien avoir à détecter.
 */
typedef enum {
    LINK_MODE_REQ_UNCHANGED = 0, /* déjà appliqué — ne rien faire, ne rien dire */
    LINK_MODE_REQ_APPLY,         /* valeur connue et nouvelle — basculer */
    LINK_MODE_REQ_REFUSE,        /* valeur inconnue — rester où l'on est, et le dire */
} link_mode_req_t;

link_mode_req_t link_proto_mode_request(uint8_t requested, uint8_t applied);

/*
 * Vrai si le bloc est celui d'une ligne flottante : uniformément 0x00 ou
 * uniformément 0xFF, selon la terminaison. Coffre absent est l'état ORDINAIRE —
 * le clavier vit sur batterie, le coffre ne s'éveille qu'en filaire.
 */
bool link_proto_is_absent(const uint8_t *regs, size_t len);
