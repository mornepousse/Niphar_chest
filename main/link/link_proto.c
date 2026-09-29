#include "link/link_proto.h"

#include <string.h>

#include "cr_crc16.h"

/* Mot magique « NIPH », octet par octet : pas de cast sur un uint32_t, qui
 * dépendrait de l'ordre des octets de la machine — or ce code tourne aussi sur
 * l'hôte, en test. */
static const uint8_t k_magic[4] = { 'N', 'I', 'P', 'H' };

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

void link_proto_pack_status(uint8_t *regs, const link_status_t *st)
{
    if (regs == NULL || st == NULL) {
        return;
    }

    memcpy(&regs[LINK_REG_MAGIC], k_magic, sizeof(k_magic));
    regs[LINK_REG_VERSION] = LINK_PROTO_VERSION;
    regs[LINK_REG_STATE] = st->state;
    put_u16(&regs[LINK_REG_PENDING_OP], st->pending_op);
    put_u32(&regs[LINK_REG_CONFIRM_COUNT], st->confirm_count);
    regs[LINK_REG_INSTANCE] = st->instance;

    /* Dernier octet du coffre avant le CRC, et plus un réservé depuis que le
     * mode actif l'occupe. Recopié tel quel, sans validation : ce qui se refuse
     * se refuse à l'application (link_proto_mode_request), et un bloc qui
     * mentirait sur son propre mode n'est pas un bloc qu'on veut voir passer
     * pour valide. */
    regs[LINK_REG_USB_MODE_ACTIVE] = st->usb_mode_active;

    /* Le libellé et sa longueur. La QUEUE EST MISE À ZÉRO, et ce n'est pas de
     * la coquetterie : un libellé plus court que le précédent laisserait sinon
     * la fin de l'ancien dans le bloc, et un maître qui ignorerait `label_len`
     * afficherait un nom composé de DEUX comptes. Le même défaut avait été
     * corrigé sur l'étiquette de remplacement OATH. */
    {
        const uint8_t n = st->label_len > LINK_LABEL_MAX ? LINK_LABEL_MAX
                                                         : st->label_len;
        regs[LINK_REG_LABEL_LEN] = n;
        memcpy(&regs[LINK_REG_LABEL], st->label, n);
        memset(&regs[LINK_REG_LABEL + n], 0x00, (size_t)(LINK_LABEL_MAX - n));
    }
    regs[LINK_REG_OP_COUNT] = st->op_count;
    regs[LINK_REG_DMA_KIND] = st->dma_kind;
    regs[LINK_REG_DMA_SEQ]  = st->dma_seq;
    put_u16(&regs[LINK_REG_DMA_LEN], st->dma_len);

    /*
     * La plage du maître (LINK_REG_MASTER_BASE, dont LINK_REG_USER_CONFIRM) lui
     * appartient : ni écrite ici, ni couverte par le CRC. L'inclure ferait
     * invalider le bloc à chaque écriture légitime du S3 ; l'écrire effacerait
     * un appui qu'il aurait posé et que le coffre n'aurait pas encore lu.
     */
    put_u16(&regs[LINK_REG_CRC], cr_crc16(regs, LINK_REG_CRC_SPAN));
}

bool link_proto_parse_status(const uint8_t *regs, size_t len, link_status_t *out)
{
    if (regs == NULL || out == NULL || len < LINK_REG_SIZE) {
        return false;
    }

    /* Ligne flottante avant tout : c'est le cas ordinaire, pas une anomalie. */
    if (link_proto_is_absent(regs, LINK_REG_SIZE)) {
        return false;
    }

    if (memcmp(&regs[LINK_REG_MAGIC], k_magic, sizeof(k_magic)) != 0) {
        return false;
    }

    /* Version inconnue : on rejette plutôt que d'interpréter de travers. Les
     * deux dépôts ne seront pas toujours flashés ensemble. */
    if (regs[LINK_REG_VERSION] != LINK_PROTO_VERSION) {
        return false;
    }

    /* Le SPI ne fournit aucune détection d'erreur : sans ce contrôle, une
     * lecture corrompue passerait pour un état plausible. */
    if (get_u16(&regs[LINK_REG_CRC]) != cr_crc16(regs, LINK_REG_CRC_SPAN)) {
        return false;
    }

    out->version = regs[LINK_REG_VERSION];
    out->state = regs[LINK_REG_STATE];
    out->pending_op = get_u16(&regs[LINK_REG_PENDING_OP]);
    out->confirm_count = get_u32(&regs[LINK_REG_CONFIRM_COUNT]);
    out->instance = regs[LINK_REG_INSTANCE];
    out->usb_mode_active = regs[LINK_REG_USB_MODE_ACTIVE];

    /* Une longueur qui déborde le champ est un bloc CORROMPU, pas un bloc à
     * tronquer : le maître lirait des octets qui ne sont pas le libellé et
     * afficherait un nom que le coffre n'a jamais composé — exactement ce que
     * ce champ existe pour empêcher. Refusé même avec un CRC juste, parce que
     * l'incohérence porte sur le SENS du bloc, pas sur sa transmission. */
    if (regs[LINK_REG_LABEL_LEN] > LINK_LABEL_MAX) {
        return false;
    }
    out->label_len = regs[LINK_REG_LABEL_LEN];
    memcpy(out->label, &regs[LINK_REG_LABEL], LINK_LABEL_MAX);
    out->op_count = regs[LINK_REG_OP_COUNT];
    out->dma_kind = regs[LINK_REG_DMA_KIND];
    out->dma_seq  = regs[LINK_REG_DMA_SEQ];
    out->dma_len  = get_u16(&regs[LINK_REG_DMA_LEN]);
    return true;
}

bool link_proto_parse_master(const uint8_t *regs, size_t len, link_master_t *out)
{
    if (regs == NULL || out == NULL || len < LINK_REG_SIZE) {
        return false;
    }

    out->confirm  = regs[LINK_REG_USER_CONFIRM];
    out->echo     = regs[LINK_REG_CONFIRM_ECHO];
    out->usb_mode = regs[LINK_REG_USB_MODE_REQ];
    out->req_seq  = regs[LINK_REG_REQ_SEQ];
    return true;
}

bool link_proto_is_absent(const uint8_t *regs, size_t len)
{
    if (regs == NULL || len == 0) {
        return true;
    }

    /*
     * Un bus sans esclave se lit uniformément — 0x00 ou 0xFF selon la
     * terminaison. Exiger l'uniformité complète : un seul octet différent
     * suffit à prouver que quelqu'un répond, et confondre un coffre bavard avec
     * une ligne morte serait pire que l'inverse.
     */
    const uint8_t first = regs[0];
    if (first != 0x00 && first != 0xFF) {
        return false;
    }
    for (size_t i = 1; i < len; i++) {
        if (regs[i] != first) {
            return false;
        }
    }
    return true;
}

bool link_proto_confirm_accepted(const link_master_t *m, uint8_t armed)
{
    if (m == NULL) {
        return false;
    }

    /*
     * LES DEUX, jamais l'un sans l'autre.
     *
     * Le motif magique filtre le bruit électrique (voir LINK_USER_CONFIRM_MAGIC)
     * ; l'écho d'instance filtre le TEMPS. La v1 n'avait que le premier, et
     * c'est ce qui lui permettait de confirmer une opération que la
     * propriétaire n'avait jamais vue : son appui pour l'opération armée à
     * l'instant T restait valable pour celle armée à T+200 ms dès lors qu'elles
     * portaient le même code.
     *
     * Un `&&` et pas deux `if` : une seule expression, donc rien à faire
     * diverger le jour où l'une des deux conditions bougera.
     */
    return m->confirm == LINK_USER_CONFIRM_MAGIC && m->echo == armed;
}

bool link_proto_usb_mode_is_known(uint8_t wire)
{
    /* Les valeurs de fil sont contiguës depuis zéro — c'est une clause du
     * contrat, pas un hasard d'écriture, et c'est ce qui autorise cette
     * comparaison à la place d'un switch qu'il faudrait tenir à jour. */
    return wire < LINK_USB_MODE_COUNT;
}

link_mode_req_t link_proto_mode_request(uint8_t requested, uint8_t applied)
{
    /*
     * L'INCONNU SE REFUSE D'ABORD, avant même de regarder si la valeur a changé.
     *
     * L'ordre n'est pas décoratif. Un octet inconnu qui se trouverait égal à la
     * dernière valeur appliquée sortirait sinon en « rien à faire » — c'est-à-dire
     * en silence, sur la seule occurrence où le coffre a quelque chose à dire.
     * Le cas ne peut pas se produire tant que rien n'applique de valeur inconnue
     * (et rien ne le fait : c'est l'autre moitié de cette fonction), mais un
     * refus qui dépend d'un invariant tenu ailleurs n'est pas un refus.
     */
    if (!link_proto_usb_mode_is_known(requested)) {
        return LINK_MODE_REQ_REFUSE;
    }

    /*
     * « Au changement », et le repère est la dernière valeur RÉELLEMENT
     * APPLIQUÉE — pas la dernière lue. C'est ce qui rend la sélection
     * auto-réparante après un redémarrage du coffre : le tampon partagé repart
     * à zéro, le maître relit 0x12, le voit différent du mode qu'il veut, le
     * réécrit, et ce changement-là est vu. Un repère sur la dernière valeur lue
     * aurait dit « inchangé » et laissé le coffre muet jusqu'à ce que la
     * propriétaire pense à changer de mode deux fois.
     */
    return requested == applied ? LINK_MODE_REQ_UNCHANGED : LINK_MODE_REQ_APPLY;
}


/* ------------------------------------------------------------------------- */
/* v3 — requête et réponses du canal DMA                                      */
/* ------------------------------------------------------------------------- */

void link_proto_pack_request(uint8_t *buf, uint8_t cmd, uint8_t arg)
{
    if (buf == NULL) {
        return;
    }
    memset(buf, 0x00, LINK_REQ_SIZE);
    buf[0] = cmd;
    buf[1] = arg;
    put_u16(&buf[LINK_REQ_SIZE - 2], cr_crc16(buf, LINK_REQ_SIZE - 2));
}

bool link_proto_parse_request(const uint8_t *buf, size_t len, link_request_t *out)
{
    if (buf == NULL || out == NULL || len < LINK_REQ_SIZE) {
        return false;
    }
    if (get_u16(&buf[LINK_REQ_SIZE - 2]) != cr_crc16(buf, LINK_REQ_SIZE - 2)) {
        return false;
    }
    /* Commande inconnue : refusée, jamais « la plus proche ». Une requête qu'on
     * n'a pas comprise est aussi fausse qu'une requête qu'on n'a pas reçue. */
    if (buf[0] != LINK_REQ_CMD_LIST && buf[0] != LINK_REQ_CMD_CODE) {
        return false;
    }
    out->cmd = buf[0];
    out->arg = buf[1];
    return true;
}

uint16_t link_proto_pack_list(uint8_t *buf, uint16_t cap,
                              uint8_t total, uint8_t first,
                              const uint8_t *idx, const char *const *noms,
                              uint8_t n, bool more)
{
    if (buf == NULL || idx == NULL || noms == NULL) {
        return 0;
    }

    /* La taille se CALCULE avant d'écrire quoi que ce soit : une écriture
     * partielle laisserait un segment que le maître lirait comme valide
     * jusqu'au CRC, et le CRC ne dirait rien puisqu'il serait juste sur ce qui
     * a été écrit. On rend zéro, et l'appelant pagine. */
    uint16_t need = LINK_LIST_HDR_SIZE + 2u;
    for (uint8_t i = 0; i < n; i++) {
        if (noms[i] == NULL) {
            return 0;
        }
        const size_t l = strlen(noms[i]);
        if (l > LINK_LABEL_MAX) {
            return 0;
        }
        need = (uint16_t)(need + 2u + l);
    }
    if (need > cap || need > LINK_DMA_MAX) {
        return 0;
    }

    buf[LINK_LIST_OFF_TOTAL] = total;
    buf[LINK_LIST_OFF_COUNT] = n;
    buf[LINK_LIST_OFF_FIRST] = first;
    buf[LINK_LIST_OFF_FLAGS] = more ? LINK_LIST_FLAG_MORE : 0x00u;

    uint16_t o = LINK_LIST_HDR_SIZE;
    for (uint8_t i = 0; i < n; i++) {
        const size_t l = strlen(noms[i]);
        buf[o++] = idx[i];
        buf[o++] = (uint8_t)l;
        memcpy(&buf[o], noms[i], l);
        o = (uint16_t)(o + l);
    }
    put_u16(&buf[o], cr_crc16(buf, o));
    return (uint16_t)(o + 2u);
}

uint16_t link_proto_pack_code(uint8_t *buf, uint16_t cap, uint8_t idx,
                              uint8_t digits, const char *code, uint8_t seconds)
{
    if (buf == NULL || code == NULL || cap < LINK_CODE_SIZE) {
        return 0;
    }
    /* Six ou huit, jamais autre chose : l'hôte comme le clavier s'en servent
     * TEL QUEL, donc une valeur inventée rendrait un code de la mauvaise
     * longueur — plausible et faux. */
    if (digits != 6u && digits != 8u) {
        return 0;
    }
    const size_t l = strlen(code);
    if (l > 8u) {
        return 0;
    }

    memset(buf, 0x00, LINK_CODE_SIZE);
    buf[0] = idx;
    buf[1] = digits;
    /* Complété À GAUCHE par des zéros : un code TOTP est une chaîne de chiffres
     * de longueur fixe, et « 0418 » n'est pas « 418 ». */
    memset(&buf[2], '0', 8);
    memcpy(&buf[2 + (8u - l)], code, l);
    buf[10] = seconds;
    put_u16(&buf[LINK_CODE_SIZE - 2], cr_crc16(buf, LINK_CODE_SIZE - 2));
    return LINK_CODE_SIZE;
}

uint8_t link_proto_format_code(uint32_t dbc, uint8_t digits, char *out)
{
    if (out == NULL || (digits != 6u && digits != 8u)) {
        return 0;
    }

    /*
     * LE MODULO EST REDONDANT, ET C'EST DIT PLUTOT QUE TU : l'ecriture de
     * droite a gauche ci-dessous ne pose que `digits` chiffres, donc elle
     * tronque deja au meme resultat. Le retirer ne change rien, et AUCUN test
     * ne peut les distinguer — verifie par mutation, qui est passee verte.
     *
     * Il reste parce qu'il nomme l'intention (« un code TOTP est un modulo
     * 10^n », RFC 4226) la ou la boucle ne montre qu'un effet de bord. Le
     * lecteur qui voudra l'enlever saura ainsi que ce n'est pas un oubli, et
     * qu'il ne casse rien — ce qui vaut mieux que de le decouvrir en lisant
     * une boucle.
     */
    uint32_t mod = 1u;
    for (uint8_t i = 0; i < digits; i++) {
        mod *= 10u;
    }
    uint32_t v = dbc % mod;

    /* Ecrit de droite a gauche : c'est ce qui donne le remplissage par des
     * zeros sans cas particulier, et sans tirer <stdio.h> dans le firmware pour
     * six chiffres. */
    out[digits] = '\0';
    for (uint8_t i = digits; i > 0u; i--) {
        out[i - 1u] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    return digits;
}
