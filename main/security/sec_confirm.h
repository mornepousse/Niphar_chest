/* main/security/sec_confirm.h — physical-keypress confirmation gate.
 * Pure (no NVS/HW); compiled for all roles so key_processor can call it. */
#pragma once
#include <stdint.h>

/* Frere sans prefixe : main/security/ est a plat sur l'include path (voir
 * main/CMakeLists.txt, INCLUDE_DIRS). Seul OATH_NAME_DISPLAY_MAX sert ici —
 * la capacite que sec_confirm_peek_labeled() exige de `out_label`. */
#include "oath_name.h"

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
    /*
     * Ajoutees APRES SEC_OP_FIDO_AUTH pour ne pas decaler ses valeurs
     * 0x05/0x06, sur lesquelles screen_op_has_deadline() est testee.
     *
     * Quatre codes et non un seul SEC_OP_OATH : oath_touch_op_t
     * (security/oath_proto.h) distingue deja CALCULATE/DELETE/REPLACE/RESET
     * cote protocole, et refuser/effacer/remplacer/tout-effacer ne se
     * refusent pas pour les memes raisons — un libelle unique pour les
     * quatre annulerait l'interet du geste : la propietaire verrait
     * « CODE OTP » alors qu'elle autorise une destruction.
     */
    SEC_OP_OATH_CODE,     /* CALCULATE : sortir un code TOTP */
    SEC_OP_OATH_DELETE,   /* DELETE : effacer un compte */
    SEC_OP_OATH_REPLACE,  /* PUT sur un nom deja present : ecrase un secret */
    SEC_OP_OATH_RESET,    /* RESET : efface tous les comptes OATH */
} sec_op_t;

void sec_confirm_reset(void);
/* Arm a pending request for `slot`/`op`, stamped at now_ms. Overwrites any
 * prior state, including an unconsumed AUTHORIZED grant. Etiquette vide :
 * equivalent a sec_confirm_arm_named(slot, op, NULL, now_ms). */
void sec_confirm_arm(uint8_t slot, sec_op_t op, uint32_t now_ms);
/*
 * Comme sec_confirm_arm(), avec une etiquette affichable en plus — le nom du
 * compte OATH (ou, pour un RESET, une forme deja mise en texte par
 * l'appelant, ex. « 12 COMPTES ») que l'appui va nommer sur l'ecran.
 *
 * `label` est assaini et tronque par oath_name_display() (security/
 * oath_name.h) avant d'etre range : jamais recopie tel quel, et jamais NULL
 * en sortie de sec_confirm_peek_labeled() meme si `label` est NULL ici.
 */
void sec_confirm_arm_named(uint8_t slot, sec_op_t op, const char *label, uint32_t now_ms);
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
/*
 * Lit l'etat, l'operation ET l'etiquette armes en UN SEUL APPEL, sans rien
 * consommer.
 *
 * Trois champs et pas deux, et un seul accesseur pour les trois — jamais
 * peek()/peek_labeled() suivi d'une lecture separee de l'etiquette. La
 * raison n'est pas seulement que deux lectures separees PEUVENT etre
 * coupees par un reset()+arm() : c'est que rien ne borne la fenetre entre
 * deux appels distincts au site d'appel. Un chargement de s_state et un
 * chargement de s_op DANS LE MEME CORPS DE FONCTION ne laissent place qu'a
 * une interruption pour s'y glisser — une fenetre etroite, mesuree en
 * cycles CPU, documentee et acceptee dans sec_confirm.c. Deux APPELS
 * separes au site d'appel laissent place a n'importe quelle ligne qu'une
 * edition future y inserera, sans qu'aucun test ne le voie : hmi.c (le seul
 * appelant) est derriere #if BOARD_CONFIRM_SOURCE, depend de FreeRTOS et du
 * GPIO, et n'entre jamais dans le harnais hote. Une fenetre fermee par la
 * LOCALITE du code est prouvee ; une fenetre fermee par « appeler la suite
 * immediatement, rien entre les deux » n'est tenue que par une discipline
 * d'ecriture — ce fichier a mis quatre corrections successives a etablir
 * cette distinction (voir CONCURRENCY MODEL, tete de sec_confirm.c), elle
 * ne se rouvre pas ici pour l'etiquette.
 *
 * DEPUIS LA CINQUIEME (revue finale de branche OATH, I3), la fenetre n'est
 * plus seulement etroite : cette fonction prend le verrou. L'argument de
 * localite ci-dessus suffisait pour s_op, un entier dont le chargement est
 * atomique — jamais pour s_label, dont la copie de douze octets peut se faire
 * couper en deux par un arm() concurrent et rendre la moitie d'une etiquette
 * collee a la moitie d'une autre. L'exigence d'UN SEUL accesseur reste :
 * elle vaut desormais parce qu'un appelant qui appellerait deux fonctions
 * verrouillees l'une apres l'autre rouvrirait la meme fenetre entre les deux.
 *
 * `out_op` peut etre NULL si l'appelant ne veut que l'etat (role de peek()).
 * `out_label`, s'il n'est pas NULL, doit pointer vers au moins
 * OATH_NAME_DISPLAY_MAX octets ecrits en une seule fois ; toujours une
 * chaine terminee, jamais NULL, y compris quand aucune etiquette n'est
 * armee.
 */
sec_confirm_state_t sec_confirm_peek_labeled(uint32_t now_ms, sec_op_t *out_op, char *out_label);
