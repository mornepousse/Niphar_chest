/* main/security/oath_proto.h — protocole YKOATH (pur, testable hote).
 *
 * Toutes les valeurs viennent de yubikit/oath.py de ykman 5.9.1.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#include "apdu.h"
#include "sec_store.h"

/* Etiquettes TLV YKOATH */
#define OATH_TAG_NAME       0x71
#define OATH_TAG_NAME_LIST  0x72
#define OATH_TAG_KEY        0x73
#define OATH_TAG_CHALLENGE  0x74
#define OATH_TAG_RESPONSE   0x75
#define OATH_TAG_TRUNCATED  0x76
#define OATH_TAG_PROPERTY   0x78
#define OATH_TAG_VERSION    0x79
#define OATH_TAG_TOUCH      0x7C

/*
 * Code dynamique de la RFC 4226 : offset dans le dernier quartet, quatre
 * octets lus a cet offset, bit de poids fort masque.
 *
 * NE FAIT PAS le modulo. ykman calcule lui-meme
 * (bytes2int(valeur) & 0x7FFFFFFF) % 10**chiffres — _format_code(), oath.py.
 * Appliquer le modulo ici rendrait des codes faux.
 */
uint32_t oath_dynamic_binary(const uint8_t *hmac, uint8_t hmac_len);

/* Cherche `tag` dans `buf`. Refuse une longueur qui deborde du tampon : la
 * trame vient de l'hote. */
bool oath_tlv_find(const uint8_t *buf, uint16_t len, uint8_t tag,
                   const uint8_t **val, uint16_t *val_len);

/* Ecrit un TLV. Rend le nombre d'octets ecrits, ou 0 si la capacite manque —
 * jamais une ecriture partielle. */
uint16_t oath_tlv_put(uint8_t *out, uint16_t cap, uint8_t tag,
                      const uint8_t *val, uint16_t val_len);

/*
 * Taille du tampon de reponse differee. Dimensionnee sur le PIRE cas que le
 * magasin peut produire, pas sur une estimation : seize slots dont chacun peut
 * porter une etiquette de 63 octets. CALCULATE ALL est le plus gros — par
 * slot, un 0x71 (2 + 63) puis un 0x7C vide (2). La fixer plus bas ferait
 * refuser un magasin plein par 6A84, c'est-a-dire refuser de lister
 * precisement quand il y a le plus a lister.
 */
#define OATH_PENDING_MAX (SEC_N_SLOTS * ((2 + SEC_LABEL_LEN - 1) + 2))

/*
 * Longueur du sel rendu dans le 0x71 du SELECT. ykman en fait un SHA-256
 * (_get_device_id) pour nommer l'appareil : il doit etre stable d'une session
 * a l'autre, ce que la tache 6 assure en le persistant en NVS.
 */
#define OATH_SALT_LEN 8

/* Longueur du defi TOTP : le compteur de pas, huit octets gros-boutiens. Toute
 * autre longueur est une trame malformee — jamais un defi a completer. */
#define OATH_CHALLENGE_LEN 8

/*
 * Le SEUL type de compte accepte : TOTP (quartet haut 0x20) sur HMAC-SHA1
 * (quartet bas 0x01). Le coffre n'embarque que cr_hmac_sha1 ; accepter un
 * compte SHA-256 — que ykman propose — le persisterait pour lui rendre
 * ETERNELLEMENT des codes faux, sans qu'aucune erreur ne le dise. Un refus
 * explicite vaut mieux qu'un mensonge silencieux.
 */
#define OATH_ALGO_TOTP_SHA1 0x21u

/*
 * Signal rendu par oath_dispatch quand la commande exige un appui physique.
 * oath_proto reste pur : c'est mode_oath.c qui appellera dongle_confirm().
 *
 * La valeur 1 ne peut pas se confondre avec une longueur de reponse : une
 * reponse porte au minimum ses deux octets de mot d'etat, et 0 est deja pris
 * par « capacite insuffisante, rien d'ecrit ».
 */
#define OATH_SW_NEEDS_TOUCH 0x0001u

/*
 * Quelle operation attend l'appui. Le code doit etre DISTINCT par operation :
 * l'ecran a dire « EFFACER » et non « CODE OTP », et un geste donne pour un
 * code n'est pas un geste donne pour une destruction.
 */
typedef enum {
    OATH_TOUCH_NONE = 0,
    OATH_TOUCH_CALCULATE,
    OATH_TOUCH_DELETE,
    OATH_TOUCH_REPLACE,   /* PUT sur un nom deja present : detruit l'ancien secret */
    OATH_TOUCH_RESET,
} oath_touch_op_t;

typedef struct {
    bool     selected;                    /* l'applet a-t-il ete selectionne ? */
    uint8_t  salt[OATH_SALT_LEN];         /* rempli par l'appelant avant le SELECT */
    uint8_t  pending[OATH_PENDING_MAX];   /* reste a envoyer par SEND REMAINING */
    uint16_t pending_len;
    uint16_t pending_off;
    /*
     * Renseignes quand oath_dispatch rend OATH_SW_NEEDS_TOUCH. Sans eux,
     * l'appelant devrait re-analyser la trame pour savoir QUEL compte et QUEL
     * defi il vient d'accepter — deux analyses de la meme donnee hote, donc
     * deux occasions de diverger sur ce qui a ete valide.
     */
    oath_touch_op_t touch_op;
    /*
     * DEPENDANCE AU TRANSPORT : `touch_slot` est un INDEX, pas une identite.
     * Entre la demande et l'appui, rien dans ce fichier n'empeche le slot
     * d'avoir change de compte — la propriete tient parce que ccid.c bloque
     * l'hote pendant l'attente de confirmation. oath_touch_commit revérifie
     * qu'il s'agit toujours d'un slot OATH, mais pas qu'il s'agit du MEME
     * compte. Quiconque rendrait cette attente non bloquante doit porter ici
     * une identite (le nom), sans quoi l'ecran afficherait un compte et un
     * autre partirait.
     */
    uint8_t  touch_slot;
    /* Combien de comptes l'operation en attente detruit. L'ecran doit le dire :
     * un seul appui pour douze secrets merite un nombre affiche. Le porter ici
     * evite a l'affichage de recompter le magasin — deux comptages du meme
     * etat sont deux occasions de diverger. */
    uint8_t  touch_count;
    uint8_t  touch_challenge[OATH_CHALLENGE_LEN];
    bool     touch_truncate;              /* P2=01 : ykman veut un 0x76 */
    /*
     * Compte mis en attente par un PUT qui ECRASE. Il porte un secret en
     * clair : oath_touch_commit et toute commande suivante l'effacent, pour
     * qu'il ne traine pas en RAM au-dela de la confirmation qu'il attend.
     */
    uint8_t  touch_put_type;
    uint8_t  touch_put_digits;
    uint8_t  touch_put_secret_len;
    uint8_t  touch_put_secret[SEC_SECRET_MAX];
    char     touch_put_label[SEC_LABEL_LEN];
} oath_ctx_t;

/*
 * Traite une commande APDU. Ecrit la reponse (mot d'etat compris) dans `out` et
 * rend sa longueur. Ne bloque jamais et ne touche a aucun peripherique.
 *
 * Rend 0 si `cap` est trop petite pour meme un mot d'etat, et
 * OATH_SW_NEEDS_TOUCH quand la reponse depend d'un appui physique.
 */
uint16_t oath_dispatch(const apdu_t *cmd, uint8_t *out, uint16_t cap,
                       oath_ctx_t *ctx);

/*
 * Execute l'operation destructrice mise en attente par oath_dispatch, une fois
 * l'appui obtenu (`granted`) ou refuse. Ecrit un mot d'etat dans `out` et rend
 * sa longueur.
 *
 * Un refus, ou l'absence de demande en attente, rend 6985 et ne touche a rien :
 * une confirmation ne peut donc pas etre rejouee, ni arriver « en avance » sur
 * une demande qui n'a pas eu lieu.
 *
 * ATTENTION — cette fonction CROIT son parametre `granted`. Elle ne verifie
 * aucun appui : elle detruit des secrets sur la foi d'un booleen. C'est le
 * decoupage voulu (oath_proto reste pur et testable, la garantie vit dans le
 * transport), mais il impose que le SEUL appelant soit le mode USB, apres un
 * appui reel et par le meme chemin d'abandon (`s_shutdown` de ccid.c) que
 * dongle_confirm(). L'appeler avec `true` depuis ailleurs — un test manuel,
 * une commande console — vide le magasin sans qu'un geste ait ete demande.
 *
 * OATH_TOUCH_CALCULATE ne passe PAS par ici et rend 0 : achever un CALCULATE
 * demande le HMAC, qui n'est pas de la logique pure et reste a l'appelant.
 */
uint16_t oath_touch_commit(oath_ctx_t *ctx, bool granted, uint8_t *out, uint16_t cap);
