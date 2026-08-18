/* main/security/fido_master.h — la cle maitresse FIDO, en NVS.
 *
 * Non pur : touche NVS et generation aleatoire materielle, donc absent de
 * test/. Voir le commentaire en tete de fido_master.c pour la decision sur
 * l'absence d'eFuse.
 */
#pragma once
#include "fido_key.h"   /* fido_hmac_fn */

/* Implementation concrete de fido_hmac_fn : HMAC-SHA256 (mbedtls) avec la
 * cle maitresse chargee (et generee au besoin) depuis NVS. Signature
 * identique a fido_hmac_fn — passable directement a fido_key_derive/
 * fido_key_tag/fido_key_check. */
void fido_master_hmac(const uint8_t *msg, size_t len, uint8_t out[32]);
