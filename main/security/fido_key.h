/* main/security/fido_key.h — derivation des cles FIDO2/U2F, en logique pure.
 *
 * Formule (docs/superpowers/specs/2026-08-17-fido2-webauthn-design.md) :
 *
 *   d   = HMAC(K_maitre, 0x01 || nonce || rp_id_hash)   cle privee du credential
 *   tag = HMAC(K_maitre, 0x02 || nonce || rp_id_hash)   son authentificateur
 *
 *   credential_id = nonce(16) || tag(16)                 32 octets
 *
 * A l'authentification on recalcule `tag` depuis le nonce presente et le
 * hash du domaine ; s'il ne correspond pas, l'identifiant n'est pas le
 * notre et on refuse (fido_key_check). Aucun identifiant n'est donc stocke
 * cote authentificateur pour les credentials non residents.
 *
 * LA SEPARATION DE DOMAINE. Les octets de prefixe 0x01/0x02 sont ce qui
 * empeche l'authentificateur du credential D'ETRE la cle privee : sans eux,
 * publier le credential ID (qui contient `tag`) reviendrait a publier `d`.
 * Ce n'est pas une precaution de style, c'est la propriete qui tient toute
 * la conception — voir test_key_and_tag_are_different_derivations dans
 * test/test_fido_key.c.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* La fonction HMAC est INJECTEE : c'est ce qui rend ce module pur et testable
 * sur l'hote, et c'est aussi ce qui permettra de passer au peripherique HMAC
 * du P4 plus tard sans toucher a cette logique. Sur cible, l'implementation
 * reelle vit dans fido_master.c (HMAC-SHA256 mbedtls, cle en NVS). */
typedef void (*fido_hmac_fn)(const uint8_t *msg, size_t len, uint8_t out[32]);

/* Derive la cle privee `d` (32 octets, scalaire ECDSA secp256r1 brut) pour
 * ce domaine et ce nonce. Ne valide pas que `d` tombe dans [1, N-1] : c'est
 * la responsabilite de l'appelant signant avec (tache 7), au meme titre que
 * mbedtls_ecdsa_sign le fait deja pour openpgp_crypto.c. */
void fido_key_derive(fido_hmac_fn hmac, const uint8_t rp_id_hash[32],
                     const uint8_t nonce[16], uint8_t d_out[32]);

/* Derive l'authentificateur `tag` (16 octets, tronque du HMAC 32 octets) qui
 * accompagnera `nonce` dans le credential ID publie a l'hote. */
void fido_key_tag(fido_hmac_fn hmac, const uint8_t rp_id_hash[32],
                  const uint8_t nonce[16], uint8_t tag_out[16]);

/* Verifie qu'un credential ID de 32 octets (nonce(16) || tag(16)) a bien ete
 * emis par CE domaine (rp_id_hash). Recalcule `tag` depuis le nonce presente
 * et compare a temps constant : un identifiant force pour un autre domaine,
 * ou altere en transit, est rejete. */
bool fido_key_check(fido_hmac_fn hmac, const uint8_t rp_id_hash[32],
                    const uint8_t cred_id[32]);
