/* main/security/sec_store.h — write-only secret slots (dongle). */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Seize slots : douze comptes TOTP de Mae, plus de la marge. Pas davantage —
 * provisionner pour un besoin qui n'existe pas coute de la NVS et du temps de
 * chargement a chaque entree dans le mode. */
#define SEC_N_SLOTS     16
/* Un nom YKOATH s'ecrit « [periode/][Issuer:]compte » : soixante-quatre octets
 * couvrent « GitHub:mae.pugin@exemple.org » sans troncature. */
#define SEC_LABEL_LEN   64
#define SEC_SECRET_MAX  64

enum { SEC_SLOT_EMPTY = 0, SEC_SLOT_HMAC_SHA1 = 1 };

typedef struct {
    uint8_t type;        /* 0 vide | 0x01 CR-HMAC | octet d'algo YKOATH */
    uint8_t flags;       /* bit0 = appui requis — force a 1 */
    uint8_t secret_len;
    uint8_t digits;      /* 6 ou 8 ; 0 tant que non renseigne */
    char    label[SEC_LABEL_LEN];
    uint8_t secret[SEC_SECRET_MAX];
} sec_slot_t;

void    sec_store_init(void);
bool    sec_store_set_slot(uint8_t idx, uint8_t type, const char *label,
                           const uint8_t *secret, uint8_t secret_len);
bool    sec_store_clear_slot(uint8_t idx);
uint8_t sec_store_count(void);
uint8_t sec_store_type(uint8_t idx);
const char *sec_store_label(uint8_t idx);
/* Nombre de chiffres du code, porte par le slot. Refuse tout ce qui n'est ni 6
 * ni 8 : RFC 4226 en admet davantage, aucun service de Mae ne s'en sert, et
 * accepter une valeur qu'on ne teste pas revient a la livrer non verifiee. */
bool    sec_store_set_digits(uint8_t idx, uint8_t digits);
uint8_t sec_store_digits(uint8_t idx);
/* INTERNAL — firmware-only, never exposed over CDC.
 * `out` must be at least SEC_SECRET_MAX bytes. */
bool    sec_store_get_secret(uint8_t idx, uint8_t *out, uint8_t *out_len);
