/* main/security/ctaphid.h — trames CTAP-HID : canaux, decoupe et
 * reassemblage des paquets init/continuation, en logique pure.
 *
 * Surface d'attaque : c'est le premier code qui touche des octets fournis
 * par l'hote (rapports HID de 64 octets). Meme rigueur qu'apdu.c — aucune
 * ecriture dans le tampon de reassemblage avant validation complete de la
 * longueur annoncee.
 *
 * Format d'un rapport HID (CTAPHID_PKT_SIZE = 64 octets) :
 *
 *   paquet d'initialisation (INIT) :
 *     octets 0-3   CID, gros-boutiste
 *     octet  4     bit 7 = 1, bits 0-6 = commande (CTAPHID_CMD_*)
 *     octets 5-6   BCNT, longueur totale du message, gros-boutiste
 *     octets 7-63  jusqu'a 57 octets de charge utile
 *
 *   paquet de continuation (CONT) :
 *     octets 0-3   CID, gros-boutiste
 *     octet  4     bit 7 = 0, bits 0-6 = numero de sequence (0..127)
 *     octets 5-63  jusqu'a 59 octets de charge utile
 *
 * CTAPHID_MAX_PAYLOAD = 57 + 128*59 = 7609 : la taille maximale qu'un
 * message peut atteindre avec un INIT suivi d'au plus 128 CONT (le
 * numero de sequence tient sur 7 bits, 0..127).
 */
#pragma once
#include <stdint.h>

#define CTAPHID_PKT_SIZE      64u
#define CTAPHID_MAX_PAYLOAD   7609u   /* 57 + 128*59 */
#define CTAPHID_CID_BROADCAST 0xFFFFFFFFu

/* commandes */
#define CTAPHID_CMD_PING   0x01u
#define CTAPHID_CMD_MSG    0x03u
#define CTAPHID_CMD_INIT   0x06u
#define CTAPHID_CMD_CBOR   0x10u
#define CTAPHID_CMD_CANCEL 0x11u
#define CTAPHID_CMD_ERROR  0x3Fu

/* erreurs */
#define CTAPHID_ERR_INVALID_CMD     0x01u
#define CTAPHID_ERR_INVALID_PAR     0x02u
#define CTAPHID_ERR_INVALID_LEN     0x03u
#define CTAPHID_ERR_INVALID_SEQ     0x04u
#define CTAPHID_ERR_CHANNEL_BUSY    0x06u
#define CTAPHID_ERR_INVALID_CHANNEL 0x0Bu
#define CTAPHID_ERR_OTHER           0x7Fu

typedef enum {
    CTAPHID_IN_PROGRESS = 0,   /* message incomplet, attendre la suite */
    CTAPHID_COMPLETE,          /* message complet dans out */
    CTAPHID_ERROR,             /* erreur : err_code et err_cid renseignes */
} ctaphid_result_t;

/*
 * Etat de reassemblage d'un message. 7,6 Kio a cause de `buf` : jamais sur
 * une pile de tache, l'appelant (tache 3) en possede une unique instance
 * statique. Ce module n'alloue rien : il travaille sur la structure fournie.
 */
typedef struct {
    uint32_t cid;              /* canal du message en cours (0 = aucun) */
    uint8_t  cmd;               /* commande du message en cours (0 = aucun) */
    uint16_t expected;         /* longueur annoncee (BCNT) */
    uint16_t got;               /* octets recus jusqu'ici */
    uint8_t  next_seq;         /* numero de sequence CONT attendu */
    uint8_t  buf[CTAPHID_MAX_PAYLOAD];
} ctaphid_asm_t;

typedef struct {
    uint32_t       cid;
    uint8_t        cmd;
    uint16_t       len;
    const uint8_t *data;       /* pointe dans a->buf, valide jusqu'au prochain feed */
    uint8_t        err_code;
    uint32_t       err_cid;
} ctaphid_msg_t;

/* Remet l'assembleur a l'etat "aucun message en cours". */
void ctaphid_reset(ctaphid_asm_t *a);

/*
 * Fait avancer le reassemblage d'un paquet de 64 octets.
 *
 * Retourne CTAPHID_COMPLETE quand le message annonce est entierement recu
 * (out->cid/cmd/len/data renseignes), CTAPHID_IN_PROGRESS s'il faut encore
 * des paquets de continuation, CTAPHID_ERROR si le paquet est refuse
 * (out->err_code/err_cid renseignes — err_cid est toujours le canal du
 * paquet fautif, celui qui a declenche l'erreur, pas necessairement le
 * canal de la transaction en cours).
 */
ctaphid_result_t ctaphid_feed(ctaphid_asm_t *a, const uint8_t pkt[CTAPHID_PKT_SIZE],
                               ctaphid_msg_t *out);

/* Prochain identifiant de canal a allouer apres `last_cid`. Jamais 0 (canal
 * interdit par la specification), jamais CTAPHID_CID_BROADCAST (diffusion,
 * jamais allouee a un client). Fonction pure : ne conserve aucun etat,
 * l'appelant garde le dernier CID alloue. */
uint32_t ctaphid_next_cid(uint32_t last_cid);
