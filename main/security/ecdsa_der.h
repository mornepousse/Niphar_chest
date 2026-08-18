/* main/security/ecdsa_der.h — encodage DER (ANSI X9.62) d'une signature
 * ECDSA P-256 brute, en logique pure.
 *
 * openpgp_crypto_p256_sign() rend r||s bruts (64 octets, chacun big-endian,
 * NON DER — c'est le format attendu par PSO:CDS OpenPGP). U2F (Raw Message
 * Format, tache 7) et CTAP2 (plan 2) exigent au contraire une signature
 * encodee DER : SEQUENCE { INTEGER r, INTEGER s }. Ce module fait cette
 * seule conversion, en pur : aucun octet ne vient de l'hote, tout vient
 * d'une signature deja calculee — donc testable sur l'hote sans mbedtls ni
 * materiel, contrairement a u2f.c qui l'appelle.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * Capacite maximale exacte d'une signature P-256 DER, PAS une marge :
 * SEQUENCE(tag 1 + longueur courte 1) + 2 x INTEGER(tag 1 + longueur 1 +
 * 32 octets + au plus 1 octet de bourrage de signe) = 2 + 2*(2+32+1) = 72.
 * La longueur du contenu de la SEQUENCE (au plus 70) tient toujours en un
 * seul octet de longueur courte (< 128), donc l'en-tete SEQUENCE fait
 * toujours 2 octets ici — jamais la forme longue.
 */
#define ECDSA_DER_MAX_LEN 72u

/*
 * Encode r||s (32 + 32 octets, big-endian, exactement ce que rend
 * openpgp_crypto_p256_sign()) en DER dans `out` (capacite `cap`, au moins
 * ECDSA_DER_MAX_LEN recommande pour ne jamais echouer). Rend le nombre
 * d'octets ecrits, ou 0 si `cap` est insuffisant ou si `r`/`s`/`out` est
 * nul — jamais de troncature silencieuse.
 */
size_t ecdsa_der_encode(const uint8_t r[32], const uint8_t s[32],
                        uint8_t *out, size_t cap);
