#include "oath_proto.h"

#include <string.h>

uint32_t oath_dynamic_binary(const uint8_t *hmac, uint8_t hmac_len)
{
    if (hmac == NULL || hmac_len < 20) return 0;
    const uint8_t off = (uint8_t)(hmac[hmac_len - 1] & 0x0Fu);
    return ((uint32_t)(hmac[off] & 0x7Fu) << 24)
         | ((uint32_t)hmac[off + 1] << 16)
         | ((uint32_t)hmac[off + 2] << 8)
         | ((uint32_t)hmac[off + 3]);
}

bool oath_tlv_find(const uint8_t *buf, uint16_t len, uint8_t tag,
                   const uint8_t **val, uint16_t *val_len)
{
    if (buf == NULL) return false;
    uint16_t i = 0;
    while ((uint16_t)(i + 2u) <= len) {
        const uint8_t t = buf[i];
        const uint8_t l = buf[i + 1];
        /* Longueur qui deborde : trame malformee, on s'arrete au lieu de lire
         * au-dela du tampon. */
        if ((uint32_t)i + 2u + l > len) return false;
        if (t == tag) {
            if (val)     *val = &buf[i + 2];
            if (val_len) *val_len = l;
            return true;
        }
        i = (uint16_t)(i + 2u + l);
    }
    return false;
}

uint16_t oath_tlv_put(uint8_t *out, uint16_t cap, uint8_t tag,
                      const uint8_t *val, uint16_t val_len)
{
    if (out == NULL || val_len > 0xFFu) return 0;
    if ((uint32_t)val_len + 2u > cap) return 0;
    out[0] = tag;
    out[1] = (uint8_t)val_len;
    if (val && val_len) memcpy(&out[2], val, val_len);
    return (uint16_t)(val_len + 2u);
}
