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
 * 3 depuis le 2026-09-29, meme journee : la v2 ne publiait AUCUNE identite.
 * `pending_op` porte un CODE d'operation, pas un compte. Sur la cle autonome
 * l'ecran est celui du coffre et affiche « CODE OTP GITHUB » ; sur le coffre,
 * l'ecran est celui du CLAVIER, qui ne recevait que « une operation de type
 * CODE OTP est en attente ». La proprietaire approuvait un TYPE, jamais un
 * COMPTE — et la decision 4 de la spec OATH existe precisement pour empecher
 * ca (« sans quoi l'appui est un interrupteur de presence et pas un accord »).
 * Elle etait tenue sur la cle et perdue sur le coffre. Le scenario qui a
 * tranche : elle demande le code de GITHUB, un hote malveillant en demande un
 * pour BANQUE dans la meme seconde, et les deux blocs sont indiscernables a
 * l'ecran. L'instance empeche l'appui de GLISSER de l'un a l'autre ; elle ne
 * dit pas lequel est affiche.
 *
 * Defauts de la v1, pour memoire :
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
#define LINK_PROTO_VERSION  3

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
 *   0x0E       longueur du libellé       coffre→S3   (0..LINK_LABEL_MAX)
 *   0x0F       nombre de comptes visés   coffre→S3
 *   0x10       type du segment en file   coffre→S3   (0 = rien en file)
 *   0x11       numéro du segment         coffre→S3
 *   0x12-0x13  longueur du segment       coffre→S3   (petit-boutiste)
 *   0x14-0x35  libellé, 34 o ASCII       coffre→S3   (non terminé par zéro)
 *   0x36-0x37  CRC16 sur 0x00..0x35      coffre→S3   (petit-boutiste)
 *   0x38       confirmation utilisateur  S3→coffre
 *   0x39       écho du numéro d'instance S3→coffre
 *   0x3A       mode USB demandé          S3→coffre
 *   0x3B       réservé                   S3→coffre
 *   0x3C       numéro de requête         S3→coffre   (sonnette DMA)
 *   0x3D-0x3F  réservé                   S3→coffre
 *
 * LA SONNETTE VIT DANS LE SECOND MOT DU MAÎTRE, ET CE N'EST PAS ESTHÉTIQUE.
 * Relevé par l'équipe KeSp, et vérifiable dans link_spi.c : le coffre efface
 * l'octet de confirmation consommé par un spi_slave_hd_write_buffer() d'UN
 * octet — mais le pilote écrit PAR MOTS DE 32 BITS, donc c'est une
 * lecture-modification-écriture sur tout le premier mot du maître. Une sonnette
 * logée dedans serait écrasée quand elle tombe dans ces quelques cycles, et la
 * requête ne serait JAMAIS servie, sans erreur nulle part. C'est la fenêtre que
 * la section 5 du contrat décrit déjà pour l'octet de mode — sauf que le mode
 * se relit et se réécrit, alors qu'une sonnette ne se relit pas.
 *
 * SOIXANTE-QUATRE octets — tout le fichier de registres partagés du P4
 * (SOC_SPI_MAXIMUM_BUFFER_SIZE) — et AUCUN mot partagé entre les extrémités.
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
#define LINK_REG_LABEL_LEN      0x0E
#define LINK_REG_OP_COUNT       0x0F
#define LINK_REG_DMA_KIND       0x10
#define LINK_REG_DMA_SEQ        0x11
#define LINK_REG_DMA_LEN        0x12
#define LINK_REG_LABEL          0x14
#define LINK_REG_CRC            0x36
#define LINK_REG_USER_CONFIRM   0x38
#define LINK_REG_CONFIRM_ECHO   0x39
#define LINK_REG_USB_MODE_REQ   0x3A
#define LINK_REG_REQ_SEQ        0x3C
#define LINK_REG_SIZE           0x40

/* Capacité du libellé sur le fil. Dimensionnée sur l'écran du CLAVIER (~48
 * caractères ASCII en UNSCII 8 sur 68×68 px) et non sur l'OLED du coffre, qui
 * n'en dessine que 21 : c'est le clavier qui affiche ce champ. Le coffre ne
 * produit que 21 caractères aujourd'hui (OATH_NAME_DISPLAY_MAX) ; l'élargir
 * sera un changement interne, sans nouvelle version de protocole. */
#define LINK_LABEL_MAX          34

/* Étendue couverte par le CRC : du début jusqu'à l'octet qui le précède. */
#define LINK_REG_CRC_SPAN       0x36

/*
 * Les deux plages de propriété, déclarées comme plages et pas comme liste
 * d'offsets : c'est sur elles que porte l'invariant du transport, et une
 * propriété se vérifie, une liste se recopie.
 *
 * Chaque plage couvre un nombre entier de mots et commence sur une frontière de
 * mot : c'est ce qui permet à chaque côté de publier le sien d'un seul bloc.
 */
#define LINK_REG_CHEST_BASE     0x00
#define LINK_REG_CHEST_LEN      0x38
#define LINK_REG_MASTER_BASE    0x38
#define LINK_REG_MASTER_LEN     0x08

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

/*
 * LINK_STATE_TIME_VALID — le coffre détient une heure murale POSÉE, jamais
 * devinée.
 *
 * Un TOTP vaut HMAC(secret, floor(unix / 30)) : sans heure, pas de code. Le
 * coffre n'a aucune horloge — ni RTC, ni pile — et la moitié gauche n'en a pas
 * davantage (RC interne, dérive en minutes par jour). L'heure vient donc de
 * l'hôte, par `niphar-oath set-time` sur le canal CCID que le client parle
 * déjà, et le coffre l'entretient en monotone.
 *
 * CE QUI REND L'INVALIDATION GRATUITE : le coffre n'existe que branché et
 * redémarre au débranchement. Son heure s'efface donc toute seule — il n'y a
 * aucun drapeau à tenir, aucune heure périmée possible.
 *
 * LE BIT NE SE LÈVE JAMAIS SUR UNE HEURE PAR DÉFAUT — ni zéro, ni l'époque de
 * compilation, ni « probablement après 2020 ». Seul un set-time reçu le lève.
 * Une heure devinée produirait des codes faux présentés comme justes, ce qui
 * est pire que pas de code du tout : le clavier affiche « NO TIME » et la
 * propriétaire sait quoi faire.
 */
#define LINK_STATE_TIME_VALID   (1u << 3)

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

    /*
     * Le NOM du compte visé par l'opération en attente, en ASCII imprimable,
     * NON terminé par zéro — `label_len` fait foi.
     *
     * C'est la raison d'être de la v3. Ce que la propriétaire voit avant
     * d'appuyer est CE champ, publié par le coffre au moment de l'armement, et
     * jamais la copie que le clavier garde de sa propre liste. Si un compte a
     * été ajouté ou effacé entre-temps et que l'index a glissé, elle voit le nom
     * du compte RÉELLEMENT visé, et n'appuie pas.
     *
     * ASCII imprimable PAR CONSTRUCTION : oath_name_display() n'accepte que
     * 0x20..0x7E et remplace tout le reste — contrôle, octet haut, UTF-8
     * multi-octets — par « ? » avant que ça n'atteigne ce champ. Le clavier
     * dessine en UNSCII, qui ne connaît pas l'UTF-8 : cette propriété lui évite
     * une substitution, et elle appartient donc au contrat.
     *
     * Vide (`label_len` à zéro) est légitime : le chemin OpenPGP ne nomme rien.
     */
    uint8_t  label_len;
    char     label[LINK_LABEL_MAX];

    /*
     * Combien de comptes l'opération en attente détruit ou touche : 0 si rien
     * n'est armé, 1 pour une opération ordinaire, N pour un RESET.
     *
     * Double une information que le libellé porte déjà en toutes lettres
     * (« 12 COMPTES »), et c'est voulu : le clavier affiche « N CPT » sans avoir
     * à analyser du français. Les deux viennent du MÊME compteur au MÊME
     * instant, donc ils ne peuvent pas diverger.
     */
    uint8_t  op_count;

    /*
     * Signalisation du canal DMA. Le maître ne doit JAMAIS lire un segment qui
     * n'est pas en file : il attend un changement de `dma_seq`, jamais un
     * `dma_kind` non nul seul. Le numéro change APRÈS la mise en file.
     */
    uint8_t  dma_kind;
    uint8_t  dma_seq;
    uint16_t dma_len;
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
    uint8_t confirm;   /* 0x38 — LINK_USER_CONFIRM_MAGIC pour un appui réel */
    uint8_t echo;      /* 0x39 — instance que le maître a lue et renvoie */
    uint8_t usb_mode;  /* 0x3A — valeur de fil du mode demandé */
    uint8_t req_seq;   /* 0x3C — sonnette : incrémentée = requête DMA prête */
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


/* ------------------------------------------------------------------------- */
/* v3 — le canal DMA : navigation et codes                                    */
/* ------------------------------------------------------------------------- */

/*
 * POURQUOI UN SECOND CANAL. Les registres partagés font 64 octets
 * (SOC_SPI_MAXIMUM_BUFFER_SIZE sur le P4) et la carte ci-dessus les occupe
 * TOUS. La navigation et les codes vivent donc sur les canaux DMA du pilote
 * esclave-HD (spi_slave_hd_queue_trans, mode segment), de taille libre.
 *
 * Commandes de fil, relevées dans components/hal/esp32p4/include/hal/spi_ll.h
 * (en mode une ligne, l'octet vaut la commande de base) : WRDMA 0x03 puis
 * WR_END 0x07 pour la requête, RDDMA 0x04 puis INT0 0x08 pour la réponse.
 *
 * Le coffre garde EN PERMANENCE une réception en file : sans réception armée,
 * un WRDMA du maître est perdu SANS ERREUR des deux côtés.
 */

/* Types de segment publiés en 0x10. Zéro veut dire « rien en file ». */
#define LINK_DMA_KIND_NONE      0x00
#define LINK_DMA_KIND_LIST      0x01
#define LINK_DMA_KIND_CODE      0x02

/* Borne d'une réponse. Le maître dimensionne son tampon dessus. */
#define LINK_DMA_MAX            512

/*
 * La requête du maître : TAILLE FIXE, multiple de quatre.
 *
 * Le pilote tronque une réception qui n'est pas un multiple de quatre — donc la
 * taille est figée par le contrat, et non déduite du contenu.
 *
 *   0x00      commande
 *   0x01      argument  (LIST : premier index ; CODE : index du compte)
 *   0x02-0x05 réservé, à zéro
 *   0x06-0x07 CRC16 sur 0x00..0x05
 *
 * Le CRC n'est pas décoratif : le canal DMA n'a aucune détection d'erreur, et
 * une commande corrompue ferait armer une confirmation pour un compte que
 * personne n'a demandé — la propriétaire verrait alors un nom qu'elle n'attend
 * pas, ce qui est le bon comportement, mais autant ne pas en arriver là.
 */
#define LINK_REQ_SIZE           8
#define LINK_REQ_CMD_LIST       0x01
#define LINK_REQ_CMD_CODE       0x02

typedef struct {
    uint8_t cmd;
    uint8_t arg;
} link_request_t;

void link_proto_pack_request(uint8_t *buf, uint8_t cmd, uint8_t arg);

/*
 * Décode une requête. Rend false — sans toucher `out` — si le tampon est trop
 * court, si le CRC ne convient pas, ou si la commande n'est pas attribuée.
 */
bool link_proto_parse_request(const uint8_t *buf, size_t len, link_request_t *out);

/*
 * En-tête d'une réponse LIST. Le TOTAL et le nombre de CETTE page sont deux
 * champs distincts pour que le clavier puisse afficher « 3/12 ».
 */
#define LINK_LIST_OFF_TOTAL     0
#define LINK_LIST_OFF_COUNT     1
#define LINK_LIST_OFF_FIRST     2
#define LINK_LIST_OFF_FLAGS     3
#define LINK_LIST_HDR_SIZE      4
#define LINK_LIST_FLAG_MORE     (1u << 0)

/*
 * Écrit une page de liste : en-tête, puis par compte index (1 o), longueur
 * (1 o), nom (n o), puis CRC16 sur tout ce qui précède.
 *
 * Rend le nombre d'octets écrits, ou ZÉRO si la capacité ne suffit pas — jamais
 * une écriture partielle, jamais un débordement. La pagination existe dès le
 * premier jour sur la suggestion de KeSp : douze comptes tiennent en une page,
 * mais quarante n'y tiendraient pas, et ça éviterait une v4.
 */
uint16_t link_proto_pack_list(uint8_t *buf, uint16_t cap,
                              uint8_t total, uint8_t first,
                              const uint8_t *idx, const char *const *noms,
                              uint8_t n, bool more);

/*
 * Réponse CODE, taille fixe :
 *   0x00      index du compte
 *   0x01      nombre de chiffres (6 ou 8)
 *   0x02-0x09 code en ASCII, complété À GAUCHE par des zéros
 *   0x0A      secondes restantes dans la fenêtre
 *   0x0B      réservé
 *   0x0C-0x0D CRC16 sur 0x00..0x0B
 *
 * Huit caractères pour le code, et pas six : un compte à huit chiffres tronqué
 * à six rendrait un code plausible et faux. C'est la même panne muette que
 * l'import par lot, qui applique un seul --digits à tout un lot.
 */
#define LINK_CODE_SIZE          14

uint16_t link_proto_pack_code(uint8_t *buf, uint16_t cap, uint8_t idx,
                              uint8_t digits, const char *code, uint8_t seconds);
