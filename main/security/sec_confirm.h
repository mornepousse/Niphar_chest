/* main/security/sec_confirm.h — physical-keypress confirmation gate.
 * Pure (no NVS/HW); compiled for all roles so key_processor can call it. */
#pragma once
#include <stdint.h>

#define SEC_CONFIRM_TIMEOUT_MS 15000u

typedef enum {
    SEC_CONFIRM_IDLE = 0,
    SEC_CONFIRM_PENDING,
    SEC_CONFIRM_AUTHORIZED,
    SEC_CONFIRM_TIMEDOUT, /* return-only signal from poll() and peek(); s_state never holds this value */
} sec_confirm_state_t;

/*
 * Ce qu'une confirmation autorise. Un CODE, pas une chaine : sec_confirm garde
 * la porte, il n'a a porter ni texte, ni allocation, ni borne de longueur a
 * defendre. La traduction en libelle affichable est une fonction pure de
 * hmi/screen_view.h, donc testable — et un futur FIDO2 ajoutera ses codes ici
 * sans toucher a ce module.
 *
 * Necessaire parce que le slot ne porte pas le sens : toutes les operations
 * CCID partagent CCID_CONFIRM_SLOT.
 */
typedef enum {
    SEC_OP_UNKNOWN = 0,   /* rien d'arme, ou origine inconnue */
    SEC_OP_SIGN,          /* PSO:CDS — signature OpenPGP */
    SEC_OP_DECRYPT,       /* PSO:DEC */
    SEC_OP_AUTH,          /* INTERNAL AUTHENTICATE */
    SEC_OP_OTP,           /* defi/reponse CR-HMAC */
    SEC_OP_FIDO_REGISTER, /* U2F_REGISTER : creer une nouvelle cle */
    SEC_OP_FIDO_AUTH,     /* U2F_AUTHENTICATE : prouver une cle existante */
} sec_op_t;

void sec_confirm_reset(void);
/* Arm a pending request for `slot`/`op`, stamped at now_ms. Overwrites any
 * prior state, including an unconsumed AUTHORIZED grant. */
void sec_confirm_arm(uint8_t slot, sec_op_t op, uint32_t now_ms);
/* Physical confirm key pressed: PENDING -> AUTHORIZED; no-op otherwise. */
void sec_confirm_authorize(uint32_t pressed_at_ms);
/* Poll at now_ms. PENDING past timeout -> returns TIMEDOUT once (then IDLE).
 * AUTHORIZED -> writes slot to *out_slot, consumes (-> IDLE), returns AUTHORIZED. */
sec_confirm_state_t sec_confirm_poll(uint32_t now_ms, uint8_t *out_slot);
/* Lit l'etat sans rien consommer ni faire avancer la machine. Pour l'affichage
 * SEULEMENT : seul poll() consomme une autorisation ou acte une expiration.
 * `now_ms` sert a signaler une echeance deja depassee sans la consommer — la
 * LED doit pouvoir montrer le refus. */
sec_confirm_state_t sec_confirm_peek(uint32_t now_ms);
/* Lit l'etat ET l'operation armee en un seul appel, sans rien consommer.
 *
 * Un seul accesseur, et pas deux, parce que deux lectures separees peuvent
 * etre coupees par un reset()+arm() : l'appelant verrait alors l'etat d'une
 * operation avec le libelle d'une autre, et l'utilisateur confirmerait en
 * croyant autoriser ce qui est affiche. C'est precisement ce que l'ecran
 * existe pour empecher.
 *
 * `out_op` peut etre NULL si l'appelant ne veut que l'etat.
 */
sec_confirm_state_t sec_confirm_peek_labeled(uint32_t now_ms, sec_op_t *out_op);
