#pragma once

/*
 * screen_anim — les trois animations de l'ecran, en logique pure.
 *
 * Une seule n'est pas decorative : la barre de decompte. Elle rend visibles les
 * quinze secondes de SEC_CONFIRM_TIMEOUT_MS, qui ne se voient nulle part
 * ailleurs — le 2026-08-17, deux generations de cles ont echoue sur des
 * expirations que rien n'annoncait.
 *
 * Toute l'arithmetique de temps est en uint32_t non signe : la difference reste
 * juste au repassage a zero du compteur de millisecondes, qui survient apres
 * ~49 jours et qu'une cle branchee en permanence atteindra.
 *
 * Les proportions sont en pour mille et non en pixels : le module ne connait
 * pas la geometrie de l'ecran, c'est screen.c qui la connait.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sec_confirm.h"

/* Decalage anti-marquage : les OLED gardent une trace permanente d'un contenu
 * statique, et cette cle peut rester branchee des journees. */
#define SCREEN_SHIFT_MAX_PX   4u
#define SCREEN_SHIFT_PERIOD_MS (60u * 1000u)   /* un pas par minute */
#define SCREEN_SLIDE_MS       400u

/* Ecran d'accueil au demarrage de la tache d'affichage : logo Niphargus et
 * version, environ 1,5 s, avant que l'ecran de repos ne prenne la main. Pas
 * de test materiel a l'oeil pour choisir cette duree — juste assez pour se
 * voir, jamais assez pour retarder la lecture des boutons en attente. */
#define SCREEN_SPLASH_MS      1500u

/* Proportion restante de la duree de confirmation, en pour mille : 1000 a
 * l'armement, 0 a l'echeance. Bornee : une barre hors bornes se dessinerait
 * n'importe ou. */
static inline uint16_t screen_bar_permille(uint32_t armed_at_ms, uint32_t now_ms)
{
    const uint32_t elapsed = now_ms - armed_at_ms;   /* juste au repassage a zero */
    if (elapsed >= SEC_CONFIRM_TIMEOUT_MS) return 0;
    return (uint16_t)(((uint64_t)(SEC_CONFIRM_TIMEOUT_MS - elapsed) * 1000u)
                      / SEC_CONFIRM_TIMEOUT_MS);
}

/* Decalage vertical courant, en pixels, borne a SCREEN_SHIFT_MAX_PX. */
static inline uint8_t screen_shift_px(uint32_t now_ms)
{
    return (uint8_t)((now_ms / SCREEN_SHIFT_PERIOD_MS) % (SCREEN_SHIFT_MAX_PX + 1u));
}

/* Avancement du glissement de bascule, en pour mille. */
static inline uint16_t screen_slide_permille(uint32_t started_ms, uint32_t now_ms)
{
    const uint32_t elapsed = now_ms - started_ms;
    if (elapsed >= SCREEN_SLIDE_MS) return 1000;
    return (uint16_t)(((uint64_t)elapsed * 1000u) / SCREEN_SLIDE_MS);
}

/* Vrai tant que l'ecran d'accueil doit rester affiche, faux des que
 * SCREEN_SPLASH_MS s'est ecoule depuis started_ms (demarrage de la tache
 * d'affichage). Meme idiome de soustraction non signee que les deux
 * fonctions ci-dessus : juste au repassage a zero du compteur de
 * millisecondes. */
static inline bool screen_splash_active(uint32_t started_ms, uint32_t now_ms)
{
    const uint32_t elapsed = now_ms - started_ms;
    return elapsed < SCREEN_SPLASH_MS;
}
