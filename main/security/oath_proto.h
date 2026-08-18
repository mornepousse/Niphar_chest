/* main/security/oath_proto.h — protocole YKOATH (pur, testable hote).
 *
 * Toutes les valeurs viennent de yubikit/oath.py de ykman 5.9.1.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

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
