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
    LED_PATTERN_STEADY,   /* couleur fixe */
    LED_PATTERN_PULSE,    /* 1 Hz — une operation attend un doigt */
    LED_PATTERN_FLASH,    /* 120 ms, puis retour a l'etat courant */
} led_pattern_t;

typedef enum {
    LED_EVENT_NONE = 0,
    LED_EVENT_GRANTED,    /* l'appui a ete pris en compte */
    LED_EVENT_REFUSED,    /* refus ou expiration */
    LED_EVENT_MODE,       /* bascule vers un nouveau mode */
} led_event_t;

typedef struct {
    led_rgb_t     rgb;
    led_pattern_t pattern;
} led_view_t;

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
        return v;
    }

    v.rgb = led_mode_colour(mode);
    if (v.rgb.r == 0 && v.rgb.g == 0 && v.rgb.b == 0) {
        v.pattern = LED_PATTERN_OFF;
        return v;
    }
    v.pattern = confirm_pending ? LED_PATTERN_PULSE : LED_PATTERN_STEADY;
    return v;
}
