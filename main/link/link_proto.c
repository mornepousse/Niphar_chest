#include "link/link_proto.h"

#include <string.h>

#include "link/cr_crc16.h"

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
    regs[LINK_REG_RESERVED] = 0x00;

    /*
     * L'octet de confirmation (LINK_REG_USER_CONFIRM) appartient au maître : ni
     * écrit ici, ni couvert par le CRC. L'inclure ferait invalider le bloc à
     * chaque écriture légitime du S3.
     */
    put_u16(&regs[LINK_REG_CRC], cr_crc16(regs, LINK_REG_CRC_SPAN));
}

bool link_proto_parse_status(const uint8_t *regs, link_status_t *out)
{
    if (regs == NULL || out == NULL) {
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
