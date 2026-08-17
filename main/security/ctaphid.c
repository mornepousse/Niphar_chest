/* main/security/ctaphid.c — trames CTAP-HID, en logique pure */
#include "ctaphid.h"
#include <stdbool.h>
#include <string.h>

void ctaphid_reset(ctaphid_asm_t *a)
{
    if (!a) return;
    a->cid       = 0;
    a->cmd       = 0;
    a->expected  = 0;
    a->got       = 0;
    a->next_seq  = 0;
    /* `buf` n'a pas besoin d'etre efface : il n'est jamais lu au-dela de
     * `expected`, et `expected` n'est valide que pendant qu'un message est
     * en cours (cmd != 0). */
}

/* Remplit `out` pour un refus, avec le canal du paquet fautif — jamais celui
 * de la transaction en cours : la specification exige qu'une erreur reponde
 * sur le canal qui l'a declenchee. */
static ctaphid_result_t refuse(ctaphid_msg_t *out, uint8_t err_code, uint32_t offending_cid)
{
    out->err_code = err_code;
    out->err_cid  = offending_cid;
    return CTAPHID_ERROR;
}

/* Livre le message reassemble et libere le canal : un seul message a la
 * fois est en vol, donc le terminer rend l'assembleur immediatement
 * disponible pour le suivant (ERR_CHANNEL_BUSY decrit l'unique alternative). */
static ctaphid_result_t deliver(ctaphid_asm_t *a, ctaphid_msg_t *out)
{
    out->cid  = a->cid;
    out->cmd  = a->cmd;
    out->len  = a->expected;
    out->data = a->buf;
    ctaphid_reset(a);
    return CTAPHID_COMPLETE;
}

ctaphid_result_t ctaphid_feed(ctaphid_asm_t *a, const uint8_t pkt[CTAPHID_PKT_SIZE],
                               ctaphid_msg_t *out)
{
    if (!a || !pkt || !out) return CTAPHID_ERROR;

    /* Comme apdu_parse : `out` part a zero avant toute autre chose. Un appel
     * qui ne termine ni ne refuse (CTAPHID_IN_PROGRESS) ne touche a aucun de
     * ces champs — sans ce memset, ils porteraient les octets laisses par
     * l'appelant sur sa pile, y compris un `data` non initialise. */
    memset(out, 0, sizeof(*out));

    uint32_t cid = ((uint32_t)pkt[0] << 24) | ((uint32_t)pkt[1] << 16)
                 | ((uint32_t)pkt[2] << 8)  |  (uint32_t)pkt[3];
    bool     is_init = (pkt[4] & 0x80u) != 0;

    if (is_init) {
        uint8_t  cmd  = (uint8_t)(pkt[4] & 0x7Fu);
        uint16_t bcnt = (uint16_t)(((uint16_t)pkt[5] << 8) | pkt[6]);

        /* Un canal DIFFERENT du notre pendant une transaction en cours est
         * occupe, pas ignore : sinon deux hotes se marcheraient dessus. */
        if (a->cmd != 0 && a->cid != cid) {
            return refuse(out, CTAPHID_ERR_CHANNEL_BUSY, cid);
        }

        /* Longueur validee AVANT toute ecriture dans a->buf : c'est le
         * debordement classique contre un authentificateur. */
        if (bcnt > CTAPHID_MAX_PAYLOAD) {
            return refuse(out, CTAPHID_ERR_INVALID_LEN, cid);
        }

        /* Soit le canal etait libre, soit c'est un INIT sur NOTRE canal en
         * cours : la specification exige que ce second cas reinitialise la
         * transaction (resynchronisation de l'hote) au lieu de l'echouer. */
        a->cid      = cid;
        a->cmd      = cmd;
        a->expected = bcnt;
        a->got      = 0;
        a->next_seq = 0;

        uint16_t room = (uint16_t)(CTAPHID_PKT_SIZE - 7u); /* 57 */
        uint16_t n    = (room < a->expected) ? room : a->expected;
        memcpy(a->buf, pkt + 7, n);
        a->got = n;

        if (a->got >= a->expected) return deliver(a, out);
        return CTAPHID_IN_PROGRESS;
    }

    /* Paquet de continuation. */
    uint8_t seq = (uint8_t)(pkt[4] & 0x7Fu);

    /* Une continuation sans initialisation prealable n'a pas de contexte :
     * aucune sequence n'est valide dans ce cas, c'est donc une sequence
     * refusee, sur le canal du paquet lui-meme puisqu'aucun autre n'existe. */
    if (a->cmd == 0) {
        return refuse(out, CTAPHID_ERR_INVALID_SEQ, cid);
    }

    /* Meme regle que pour un INIT : un canal different pendant une
     * transaction en cours est occupe, pas ignore. */
    if (a->cid != cid) {
        return refuse(out, CTAPHID_ERR_CHANNEL_BUSY, cid);
    }

    if (seq != a->next_seq) {
        return refuse(out, CTAPHID_ERR_INVALID_SEQ, cid);
    }

    /* Bornage a CHAQUE continuation : `got + n` ne depasse jamais
     * `expected`, meme si le paquet transporte plus que le message n'en a
     * annonce. */
    uint16_t room      = (uint16_t)(CTAPHID_PKT_SIZE - 5u); /* 59 */
    uint16_t remaining = (uint16_t)(a->expected - a->got);
    uint16_t n         = (room < remaining) ? room : remaining;
    memcpy(a->buf + a->got, pkt + 5, n);
    a->got = (uint16_t)(a->got + n);
    a->next_seq++;

    if (a->got >= a->expected) return deliver(a, out);
    return CTAPHID_IN_PROGRESS;
}

uint32_t ctaphid_next_cid(uint32_t last_cid)
{
    uint32_t next = last_cid + 1u;
    if (next == 0u || next == CTAPHID_CID_BROADCAST) next = 1u;
    return next;
}
