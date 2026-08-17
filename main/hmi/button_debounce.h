#pragma once

/*
 * button_debounce — un niveau brut echantillonne, des fronts stables.
 *
 * Pur, sans etat global : chaque bouton a son instance, ce qui rend les tests
 * parallelisables et rend impossible qu'un bouton perturbe l'autre.
 *
 * Aucune notion d'appui long ici, et c'est un choix de conception, pas un
 * oubli : la carte-cle a DEUX boutons, un par metier. Avec un seul bouton, le
 * seuil de duree serait la seule chose separant « je confirme cette signature »
 * de « je desinstalle le CCID pendant que l'hote s'en sert » — voir la spec.
 *
 * L'arithmetique de temps est faite en uint32_t non signe : la difference
 * reste juste au repassage a zero du compteur de millisecondes, qui survient
 * apres ~49 jours et qu'une cle branchee en permanence atteindra.
 */

#include <stdbool.h>
#include <stdint.h>

/* 20 ms : au-dela de la duree de rebond d'un contact mecanique courant, et
 * bien en deca du temps de reaction humain, donc invisible a l'usage. */
#define BTN_DEBOUNCE_MS 20u

typedef enum {
    BTN_NONE = 0,   /* rien de stable a signaler */
    BTN_PRESSED,    /* front stable vers l'appui */
    BTN_RELEASED,   /* front stable vers le relachement */
} btn_event_t;

typedef struct {
    bool     stable;     /* dernier niveau reconnu comme stable */
    bool     candidate;  /* niveau vu au dernier echantillon */
    uint32_t since_ms;   /* depuis quand le candidat tient */
} btn_debounce_t;

static inline void btn_init(btn_debounce_t *b)
{
    b->stable    = false;   /* relache : l'etat sur au demarrage */
    b->candidate = false;
    b->since_ms  = 0;
}

static inline btn_event_t btn_feed(btn_debounce_t *b, bool pressed, uint32_t now_ms)
{
    if (pressed != b->candidate) {
        /* La ligne a bouge : le compte a rebours repart. C'est ce qui absorbe
         * le rebond, y compris une micro-coupure en plein maintien. */
        b->candidate = pressed;
        b->since_ms  = now_ms;
        return BTN_NONE;
    }
    if (pressed == b->stable) {
        return BTN_NONE;   /* rien de neuf : c'est le cas du maintien long */
    }
    if ((uint32_t)(now_ms - b->since_ms) < BTN_DEBOUNCE_MS) {
        return BTN_NONE;   /* stable, mais pas encore assez longtemps */
    }
    b->stable = pressed;
    return pressed ? BTN_PRESSED : BTN_RELEASED;
}
