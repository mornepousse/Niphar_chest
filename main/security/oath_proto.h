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
 * Signal rendu par oath_dispatch quand la commande exige un appui physique.
 * oath_proto reste pur : c'est mode_oath.c qui appellera dongle_confirm().
 *
 * La valeur 1 ne peut pas se confondre avec une longueur de reponse : une
 * reponse porte au minimum ses deux octets de mot d'etat, et 0 est deja pris
 * par « capacite insuffisante, rien d'ecrit ».
 */
#define OATH_SW_NEEDS_TOUCH 0x0001u

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
    uint8_t  touch_slot;
    uint8_t  touch_challenge[OATH_CHALLENGE_LEN];
    bool     touch_truncate;              /* P2=01 : ykman veut un 0x76 */
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
