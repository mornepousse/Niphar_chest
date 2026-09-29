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
