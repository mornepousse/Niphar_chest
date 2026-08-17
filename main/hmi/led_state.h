#pragma once

/*
 * led_state — ce que la LED a le droit de dire, et rien d'autre.
 *
 * Pur : le pilotage RMT vit dans hmi/hmi.c. Ce fichier ne fait que decider
 * quelle couleur et quel motif correspondent a un etat.
 *
 * Le vocabulaire compte plus que l'esthetique. La LED doit dire QUAND elle
 * attend un doigt, sinon l'appui devient une devinette et la porte de presence
 * physique ne protege plus rien. Elle doit aussi distinguer un refus d'une
 * panne — sans quoi un PIN faux et une cle morte se ressemblent.
 */

#include <stdbool.h>
#include <stdint.h>

#include "usb/usb_mode.h"

/* Discret au repos, visible sur un verdict. C'est une cle, pas une lampe. */
#define LED_DIM    20u
#define LED_BRIGHT 120u

typedef struct { uint8_t r, g, b; } led_rgb_t;

typedef enum {
    LED_PATTERN_OFF = 0,
    LED_PATTERN_STEADY,      /* couleur fixe */
    LED_PATTERN_ALTERNATE,   /* 1 Hz — une operation attend un doigt */
    LED_PATTERN_FLASH,       /* 120 ms, puis retour a l'etat courant */
} led_pattern_t;

typedef enum {
    LED_EVENT_NONE = 0,
    LED_EVENT_GRANTED,    /* l'appui a ete pris en compte */
    LED_EVENT_REFUSED,    /* refus ou expiration */
    LED_EVENT_MODE,       /* bascule vers un nouveau mode */
} led_event_t;

typedef struct {
    led_rgb_t     rgb;       /* couleur principale */
    led_rgb_t     rgb_alt;   /* seconde couleur de l'alternance ; egale a
                               * rgb hors attente, pour qu'un consommateur qui
                               * ignorerait le motif ne clignote jamais. */
    led_pattern_t pattern;
} led_view_t;

/* 1 Hz : periode de l'alternance de la LED pendant une attente. */
#define LED_ALTERNATE_MS 1000u

typedef enum {
    LED_WAIT_PHASE_PRIMARY = 0,   /* montrer rgb — la couleur du mode */
    LED_WAIT_PHASE_ALT,           /* montrer rgb_alt — le rouge de l'attente */
} led_wait_phase_t;

/*
 * Quelle couleur de l'alternance montrer, a partir de l'instant d'ARMEMENT
 * de l'attente (pas de l'horloge murale).
 *
 * Avant cette fonction, hmi.c dérivait la phase de `t % LED_ALTERNATE_MS`
 * directement sur l'horloge murale. C'est faux : une attente qui s'arme
 * dans la seconde moitie d'une periode demarre alors sur rgb_alt (le
 * rouge) — exactement le cas a eviter, puisque le rouge porte deja le sens
 * « refuse » ailleurs (le flash de verdict). La garantie que cette fonction
 * porte : la premiere phase vue par l'utilisateur, quel que soit l'instant
 * d'armement, est TOUJOURS LED_WAIT_PHASE_PRIMARY.
 *
 * `now_ms` doit avoir ete observe apres (ou au meme instant que) `armed_at_ms` ;
 * la soustraction est faite en uint32_t non signe, ce qui reste juste meme au
 * repassage a zero du compteur de millisecondes (~49 jours), sur le meme
 * principe que hmi/button_debounce.h.
 */
static inline led_wait_phase_t led_wait_phase(uint32_t armed_at_ms, uint32_t now_ms)
{
    const uint32_t elapsed = now_ms - armed_at_ms;
    return (elapsed % LED_ALTERNATE_MS < LED_ALTERNATE_MS / 2)
               ? LED_WAIT_PHASE_PRIMARY : LED_WAIT_PHASE_ALT;
}

static inline led_rgb_t led_mode_colour(usb_mode_t mode)
{
    switch (mode) {
    case USB_MODE_PGP:     return (led_rgb_t){ 0,       0,       LED_DIM };
    case USB_MODE_OTP:     return (led_rgb_t){ 0,       LED_DIM, 0       };
    case USB_MODE_STORAGE: return (led_rgb_t){ LED_DIM, LED_DIM, 0       };
    /* NONE et toute valeur aberrante : eteint. Le defaut sur est l'obscurite,
     * pour la meme raison que USB_MODE_NONE vaut zero. */
    default:               return (led_rgb_t){ 0,       0,       0       };
    }
}

static inline led_view_t led_state_view(usb_mode_t mode,
                                        bool confirm_pending,
                                        led_event_t event)
{
    led_view_t v;

    if (event != LED_EVENT_NONE) {
        /* Un verdict prime sur tout le reste, y compris sur l'attente : c'est
         * fugace, et c'est ce que l'utilisateur cherche a lire a cet instant. */
        v.pattern = LED_PATTERN_FLASH;
        switch (event) {
        case LED_EVENT_GRANTED:
            v.rgb = (led_rgb_t){ LED_BRIGHT, LED_BRIGHT, LED_BRIGHT };
            break;
        case LED_EVENT_REFUSED:
            v.rgb = (led_rgb_t){ LED_BRIGHT, 0, 0 };
            break;
        default: {
            /* Bascule : la couleur du NOUVEAU mode, montee en luminosite. */
            const led_rgb_t c = led_mode_colour(mode);
            v.rgb = (led_rgb_t){ c.r ? LED_BRIGHT : 0,
                                 c.g ? LED_BRIGHT : 0,
                                 c.b ? LED_BRIGHT : 0 };
            break;
        }
        }
        v.rgb_alt = v.rgb;
        return v;
    }

    v.rgb = led_mode_colour(mode);
    v.rgb_alt = v.rgb;
    if (v.rgb.r == 0 && v.rgb.g == 0 && v.rgb.b == 0) {
        v.pattern = LED_PATTERN_OFF;
        return v;
    }

    if (confirm_pending) {
        /* Constat sur materiel : une pulsation de luminosite (0 a LED_DIM)
         * est trop discrete, elle se rate si on ne fixe pas la LED. On
         * alterne donc franchement entre la couleur du mode a pleine
         * luminosite et le rouge, egalement a pleine luminosite. Le rouge
         * porte deja le refus ailleurs (flash de 120 ms) ; c'est une reserve
         * assumee, distinguee par la duree — 15 s d'attente contre 120 ms de
         * refus. */
        v.pattern = LED_PATTERN_ALTERNATE;
        v.rgb     = (led_rgb_t){ v.rgb.r ? LED_BRIGHT : 0,
                                 v.rgb.g ? LED_BRIGHT : 0,
                                 v.rgb.b ? LED_BRIGHT : 0 };
        v.rgb_alt = (led_rgb_t){ LED_BRIGHT, 0, 0 };
        return v;
    }

    v.pattern = LED_PATTERN_STEADY;
    return v;
}
