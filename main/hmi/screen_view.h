#pragma once

/*
 * screen_view — quel ecran l'utilisateur voit, et avec quels mots.
 *
 * Pur : le pilote SSD1306 et la tache d'affichage vivent dans hmi/screen.c.
 * Ce fichier ne fait que decider.
 *
 * C'est le module qui porte le seul gain de securite de l'ecran : dire POUR
 * QUOI on appuie. Un bouton sans ecran prouve qu'un humain a touche, jamais
 * son consentement a CETTE operation-la — un hote peut demander une signature
 * pendant que l'utilisateur croit en confirmer une autre. D'ou l'exigence que
 * deux operations ne partagent jamais un libelle, verifiee par un test.
 */

#include <stdbool.h>

#include "sec_confirm.h"
#include "usb/usb_mode.h"
#include "hmi/led_state.h"

typedef enum {
    SCREEN_IDLE = 0,   /* le mode, au repos */
    SCREEN_WAIT,       /* une operation attend un appui */
    SCREEN_VERDICT,    /* accorde / refuse, fugace */
    SCREEN_SWITCH,     /* bascule de mode, fugace */
} screen_kind_t;

typedef struct {
    screen_kind_t kind;
    const char   *title;   /* jamais NULL */
    const char   *line;    /* jamais NULL ; le libelle de l'operation en attente */
} screen_view_t;

static inline const char *screen_op_label(sec_op_t op)
{
    switch (op) {
    case SEC_OP_SIGN:    return "Signature OpenPGP";
    case SEC_OP_DECRYPT: return "Dechiffrement";
    case SEC_OP_AUTH:    return "Authentification";
    case SEC_OP_OTP:     return "Cle OTP";
    case SEC_OP_FIDO_REGISTER: return "Creation de cle FIDO";
    case SEC_OP_FIDO_AUTH:     return "Authentification FIDO";
    case SEC_OP_OATH_CODE:     return "Code OTP";
    case SEC_OP_OATH_DELETE:   return "Effacer";
    case SEC_OP_OATH_REPLACE:  return "Remplacer";
    case SEC_OP_OATH_RESET:    return "Tout effacer";
    /* Une origine inconnue se dit, elle ne s'habille pas du libelle d'une
     * operation connue : mieux vaut « operation inconnue » qu'un mensonge
     * plausible. */
    default:             return "Operation inconnue";
    }
}

static inline const char *screen_mode_name(usb_mode_t mode)
{
    switch (mode) {
    case USB_MODE_NONE:    return "Au repos";
    case USB_MODE_PGP:     return "OpenPGP";
    case USB_MODE_OTP:     return "Cle OTP";
    case USB_MODE_FIDO:    return "Cle FIDO2";
    /* « TOTP » et non « Comptes TOTP » : ce nom voyage avec le logo errant de
     * la veille (draw_text() dans draw_logo_wander, screen.c), donc il doit
     * rester ETROIT — le groupe logo+libelle se promene sur toute la dalle, et
     * un libelle large reduirait sa course a rien. Ce n'est PAS une contrainte
     * de police double hauteur : le mode s'ecrit en police simple, ici comme
     * dans le bandeau. (La justification precedente invoquait « dix caracteres
     * en double hauteur » — la meme erreur de police que celle corrigee sur
     * OATH_NAME_DISPLAY_MAX, voir security/oath_name.h.) Distinct de
     * « Cle OTP » juste au-dessus, qui designe le defi/reponse CR-HMAC — deux
     * modes voisins par le nom mais pas par le protocole. */
    case USB_MODE_OATH:    return "TOTP";
    case USB_MODE_STORAGE: return "Disque";
    /* Une valeur hors enum (USB_MODE_COUNT, ou un futur mode ajoute avant
     * COUNT sans mise a jour de ce switch) ne doit pas se faire passer pour
     * le repos : meme principe que screen_op_label() pour SEC_OP_UNKNOWN,
     * pour la meme raison — un silence identique a un etat connu masquerait
     * l'erreur au lieu de la signaler. */
    default:               return "Mode inconnu";
    }
}

static inline screen_view_t screen_view_of(usb_mode_t mode,
                                           bool confirm_pending,
                                           sec_op_t op,
                                           led_event_t event)
{
    screen_view_t v;

    /* L'attente d'abord — avant meme un verdict encore actif (event ==
     * LED_EVENT_GRANTED/REFUSED) : c'est le seul ecran qui reclame une
     * action de l'utilisateur, et le rater lui coute une operation entiere,
     * alors qu'un verdict rate ne coute qu'une information deja portee par
     * la LED. Consequence assumee : si une confirmation s'arme pendant
     * qu'un verdict precedent est encore affiche, ce verdict n'apparait
     * jamais a l'ecran. Voir test_wait_beats_pending_verdict. */
    if (confirm_pending) {
        v.kind  = SCREEN_WAIT;
        v.title = "CONFIRMER ?";
        v.line  = screen_op_label(op);
        return v;
    }

    if (event == LED_EVENT_GRANTED || event == LED_EVENT_REFUSED) {
        v.kind  = SCREEN_VERDICT;
        v.title = (event == LED_EVENT_GRANTED) ? "ACCORDE" : "REFUSE";
        v.line  = screen_mode_name(mode);
        return v;
    }

    if (event == LED_EVENT_MODE) {
        v.kind  = SCREEN_SWITCH;
        v.title = screen_mode_name(mode);
        v.line  = "";
        return v;
    }

    v.kind  = SCREEN_IDLE;
    v.title = screen_mode_name(mode);
    v.line  = "";
    return v;
}
