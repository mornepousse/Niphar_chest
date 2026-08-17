#include "hmi/screen.h"

#include "board.h"

#if defined(BOARD_OLED_SCL)

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hmi/hmi.h"
#include "hmi/screen_anim.h"
#include "hmi/screen_layout.h"
#include "hmi/screen_logo.h"
#include "hmi/screen_view.h"

static const char *TAG = "screen";

/* 20 images/s : assez fluide pour la barre de decompte et le glissement de
 * bascule, assez lent pour laisser toute la marge de bus a la tache IHM —
 * voir le commentaire de tete de screen.h sur pourquoi c'est une tache a
 * part. */
#define SCREEN_TASK_MS           50u
#define SCREEN_TASK_STACK        3072
#define SCREEN_TASK_PRIO         4     /* une de moins que la tache IHM (5) */
#define SCREEN_I2C_TIMEOUT_MS    100

#define SCREEN_PAGES  (BOARD_OLED_HEIGHT / 8u)

/* Cellule de la police double hauteur : le double de SCREEN_CHAR_PX
 * (hmi/screen_layout.h), qui est la mesure dont se sert screen_text_px(). Les
 * deux doivent avancer du meme pas, d'ou la derivation plutot qu'un 12 en dur. */
#define SCREEN_BIG_CHAR_PX  (2u * SCREEN_CHAR_PX)

/* Hauteur du bandeau inverse : 7 lignes de glyphe plus une marge de chaque
 * cote, arrondi a la page suivante. */
#define SCREEN_BAND_H       10

/* Echecs I2C consecutifs avant de rejouer la sequence d'init : 20 ticks de
 * 50 ms, soit une seconde. Assez long pour ignorer une trame perdue isolee,
 * assez court pour que la dalle revienne avant qu'un humain renonce. */
#define SCREEN_REINIT_AFTER_FAILS 20u

/* Instant du rapport de marge de pile, apres l'ecran d'accueil : a ce moment les
 * chemins les plus profonds (logo, police double hauteur, veille) ont tous ete
 * empruntes au moins une fois, donc la mesure porte sur le pire cas reel et non
 * sur une estimation. */
#define SCREEN_HWM_REPORT_MS 10000u

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t                 s_fb[BOARD_OLED_WIDTH * BOARD_OLED_HEIGHT / 8u];

/* ------------------------------------------------------------------------- */
/* Police maison 5x7, dans une cellule 6x8 (5 colonnes + 1 de marge, 7 lignes */
/* + 1 de marge).                                                             */
/* ------------------------------------------------------------------------- */

/*
 * Dessinee a la main pour ce projet plutot que reprise d'une table tierce
 * dont l'exactitude ne peut se verifier que sur l'ecran reel — voir le
 * dessin ASCII juste au-dessus de chaque glyphe ('#' = pixel allume, '.' =
 * eteint) : c'est la meme grille que R() encode, donc une revue peut
 * comparer les deux sans materiel. Reste a confirmer a l'oeil sur l'ecran
 * (voir le rapport de tache).
 *
 * Un caractere hors de [0x20 (espace), 'z'] retombe sur le glyphe '?'
 * (glyph_for()). Un caractere DANS cet intervalle mais non dessine ici
 * (ponctuation ASCII hors « ? », non demandee par le brief) retombe sur un
 * glyphe vide, indiscernable de l'espace — sans consequence : aucune chaine
 * de hmi/screen_view.h n'emet un tel caractere.
 */
typedef struct { uint8_t rows[7]; } screen_glyph_t;

#define R(a, b, c, d, e) (uint8_t)(((a) << 4) | ((b) << 3) | ((c) << 2) | ((d) << 1) | (e))

static const screen_glyph_t s_font['z' - ' ' + 1] = {
    /* espace : laissee au zero-init par defaut, deja le bon glyphe. */

    ['0' - ' '] = {{ R(0,1,1,1,0), R(1,0,0,0,1), R(1,0,0,1,1), R(1,0,1,0,1), R(1,1,0,0,1), R(1,0,0,0,1), R(0,1,1,1,0) }},
    ['1' - ' '] = {{ R(0,0,1,0,0), R(0,1,1,0,0), R(0,0,1,0,0), R(0,0,1,0,0), R(0,0,1,0,0), R(0,0,1,0,0), R(0,1,1,1,0) }},
    ['2' - ' '] = {{ R(0,1,1,1,0), R(1,0,0,0,1), R(0,0,0,0,1), R(0,0,0,1,0), R(0,0,1,0,0), R(0,1,0,0,0), R(1,1,1,1,1) }},
    ['3' - ' '] = {{ R(0,1,1,1,0), R(1,0,0,0,1), R(0,0,0,0,1), R(0,0,1,1,0), R(0,0,0,0,1), R(1,0,0,0,1), R(0,1,1,1,0) }},
    ['4' - ' '] = {{ R(0,0,0,1,0), R(0,0,1,1,0), R(0,1,0,1,0), R(1,0,0,1,0), R(1,1,1,1,1), R(0,0,0,1,0), R(0,0,0,1,0) }},
    ['5' - ' '] = {{ R(1,1,1,1,1), R(1,0,0,0,0), R(1,1,1,1,0), R(0,0,0,0,1), R(0,0,0,0,1), R(1,0,0,0,1), R(0,1,1,1,0) }},
    ['6' - ' '] = {{ R(0,0,1,1,0), R(0,1,0,0,0), R(1,0,0,0,0), R(1,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,1), R(0,1,1,1,0) }},
    ['7' - ' '] = {{ R(1,1,1,1,1), R(0,0,0,0,1), R(0,0,0,1,0), R(0,0,1,0,0), R(0,1,0,0,0), R(0,1,0,0,0), R(0,1,0,0,0) }},
    ['8' - ' '] = {{ R(0,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,1), R(0,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,1), R(0,1,1,1,0) }},
    ['9' - ' '] = {{ R(0,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,1), R(0,1,1,1,1), R(0,0,0,0,1), R(0,0,0,1,0), R(0,1,1,0,0) }},

    ['A' - ' '] = {{ R(0,0,1,0,0), R(0,1,0,1,0), R(1,0,0,0,1), R(1,0,0,0,1), R(1,1,1,1,1), R(1,0,0,0,1), R(1,0,0,0,1) }},
    ['B' - ' '] = {{ R(1,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,1), R(1,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,1), R(1,1,1,1,0) }},
    ['C' - ' '] = {{ R(0,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,0), R(1,0,0,0,0), R(1,0,0,0,0), R(1,0,0,0,1), R(0,1,1,1,0) }},
    ['D' - ' '] = {{ R(1,1,1,0,0), R(1,0,0,1,0), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,1,0), R(1,1,1,0,0) }},
    ['E' - ' '] = {{ R(1,1,1,1,1), R(1,0,0,0,0), R(1,0,0,0,0), R(1,1,1,1,0), R(1,0,0,0,0), R(1,0,0,0,0), R(1,1,1,1,1) }},
    ['F' - ' '] = {{ R(1,1,1,1,1), R(1,0,0,0,0), R(1,0,0,0,0), R(1,1,1,1,0), R(1,0,0,0,0), R(1,0,0,0,0), R(1,0,0,0,0) }},
    ['G' - ' '] = {{ R(0,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,0), R(1,0,1,1,1), R(1,0,0,0,1), R(1,0,0,0,1), R(0,1,1,1,0) }},
    ['H' - ' '] = {{ R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(1,1,1,1,1), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1) }},
    ['I' - ' '] = {{ R(0,1,1,1,0), R(0,0,1,0,0), R(0,0,1,0,0), R(0,0,1,0,0), R(0,0,1,0,0), R(0,0,1,0,0), R(0,1,1,1,0) }},
    ['J' - ' '] = {{ R(0,0,1,1,1), R(0,0,0,1,0), R(0,0,0,1,0), R(0,0,0,1,0), R(0,0,0,1,0), R(1,0,0,1,0), R(0,1,1,0,0) }},
    ['K' - ' '] = {{ R(1,0,0,0,1), R(1,0,0,1,0), R(1,0,1,0,0), R(1,1,0,0,0), R(1,0,1,0,0), R(1,0,0,1,0), R(1,0,0,0,1) }},
    ['L' - ' '] = {{ R(1,0,0,0,0), R(1,0,0,0,0), R(1,0,0,0,0), R(1,0,0,0,0), R(1,0,0,0,0), R(1,0,0,0,0), R(1,1,1,1,1) }},
    ['M' - ' '] = {{ R(1,0,0,0,1), R(1,1,0,1,1), R(1,0,1,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1) }},
    ['N' - ' '] = {{ R(1,0,0,0,1), R(1,1,0,0,1), R(1,0,1,0,1), R(1,0,0,1,1), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1) }},
    ['O' - ' '] = {{ R(0,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(0,1,1,1,0) }},
    ['P' - ' '] = {{ R(1,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,1), R(1,1,1,1,0), R(1,0,0,0,0), R(1,0,0,0,0), R(1,0,0,0,0) }},
    ['Q' - ' '] = {{ R(0,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,1,0,1), R(1,0,0,1,0), R(0,1,1,0,1) }},
    ['R' - ' '] = {{ R(1,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,1), R(1,1,1,1,0), R(1,0,1,0,0), R(1,0,0,1,0), R(1,0,0,0,1) }},
    ['S' - ' '] = {{ R(0,1,1,1,1), R(1,0,0,0,0), R(1,0,0,0,0), R(0,1,1,1,0), R(0,0,0,0,1), R(0,0,0,0,1), R(1,1,1,1,0) }},
    ['T' - ' '] = {{ R(1,1,1,1,1), R(0,0,1,0,0), R(0,0,1,0,0), R(0,0,1,0,0), R(0,0,1,0,0), R(0,0,1,0,0), R(0,0,1,0,0) }},
    ['U' - ' '] = {{ R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(0,1,1,1,0) }},
    ['V' - ' '] = {{ R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(0,1,0,1,0), R(0,0,1,0,0) }},
    ['W' - ' '] = {{ R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,1,0,1), R(1,0,1,0,1), R(1,1,0,1,1), R(1,0,0,0,1) }},
    ['X' - ' '] = {{ R(1,0,0,0,1), R(0,1,0,1,0), R(0,0,1,0,0), R(0,0,1,0,0), R(0,0,1,0,0), R(0,1,0,1,0), R(1,0,0,0,1) }},
    ['Y' - ' '] = {{ R(1,0,0,0,1), R(0,1,0,1,0), R(0,0,1,0,0), R(0,0,1,0,0), R(0,0,1,0,0), R(0,0,1,0,0), R(0,0,1,0,0) }},
    ['Z' - ' '] = {{ R(1,1,1,1,1), R(0,0,0,0,1), R(0,0,0,1,0), R(0,0,1,0,0), R(0,1,0,0,0), R(1,0,0,0,0), R(1,1,1,1,1) }},

    ['a' - ' '] = {{ R(0,0,0,0,0), R(0,0,0,0,0), R(0,1,1,1,0), R(0,0,0,0,1), R(0,1,1,1,1), R(1,0,0,0,1), R(0,1,1,1,1) }},
    ['b' - ' '] = {{ R(1,0,0,0,0), R(1,0,0,0,0), R(1,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(1,1,1,1,0) }},
    ['c' - ' '] = {{ R(0,0,0,0,0), R(0,0,0,0,0), R(0,1,1,1,0), R(1,0,0,0,0), R(1,0,0,0,0), R(1,0,0,0,0), R(0,1,1,1,0) }},
    ['d' - ' '] = {{ R(0,0,0,0,1), R(0,0,0,0,1), R(0,1,1,1,1), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(0,1,1,1,1) }},
    ['e' - ' '] = {{ R(0,0,0,0,0), R(0,0,0,0,0), R(0,1,1,1,0), R(1,0,0,0,1), R(1,1,1,1,1), R(1,0,0,0,0), R(0,1,1,1,0) }},
    ['f' - ' '] = {{ R(0,0,1,1,0), R(0,1,0,0,0), R(0,1,0,0,0), R(1,1,1,0,0), R(0,1,0,0,0), R(0,1,0,0,0), R(0,1,0,0,0) }},
    ['g' - ' '] = {{ R(0,0,0,0,0), R(0,1,1,1,1), R(1,0,0,0,1), R(1,0,0,0,1), R(0,1,1,1,1), R(0,0,0,0,1), R(0,1,1,1,0) }},
    ['h' - ' '] = {{ R(1,0,0,0,0), R(1,0,0,0,0), R(1,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1) }},
    ['i' - ' '] = {{ R(0,0,1,0,0), R(0,0,0,0,0), R(0,1,1,0,0), R(0,0,1,0,0), R(0,0,1,0,0), R(0,0,1,0,0), R(0,1,1,1,0) }},
    ['j' - ' '] = {{ R(0,0,0,1,0), R(0,0,0,0,0), R(0,0,1,1,0), R(0,0,0,1,0), R(0,0,0,1,0), R(1,0,0,1,0), R(0,1,1,0,0) }},
    ['k' - ' '] = {{ R(1,0,0,0,0), R(1,0,0,0,0), R(1,0,0,1,0), R(1,0,1,0,0), R(1,1,0,0,0), R(1,0,1,0,0), R(1,0,0,1,0) }},
    ['l' - ' '] = {{ R(0,1,1,0,0), R(0,0,1,0,0), R(0,0,1,0,0), R(0,0,1,0,0), R(0,0,1,0,0), R(0,0,1,0,0), R(0,1,1,1,0) }},
    ['m' - ' '] = {{ R(0,0,0,0,0), R(0,0,0,0,0), R(1,1,0,1,0), R(1,0,1,0,1), R(1,0,1,0,1), R(1,0,0,0,1), R(1,0,0,0,1) }},
    ['n' - ' '] = {{ R(0,0,0,0,0), R(0,0,0,0,0), R(1,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1) }},
    ['o' - ' '] = {{ R(0,0,0,0,0), R(0,0,0,0,0), R(0,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(0,1,1,1,0) }},
    ['p' - ' '] = {{ R(0,0,0,0,0), R(1,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,1), R(1,1,1,1,0), R(1,0,0,0,0), R(1,0,0,0,0) }},
    ['q' - ' '] = {{ R(0,0,0,0,0), R(0,1,1,1,1), R(1,0,0,0,1), R(1,0,0,0,1), R(0,1,1,1,1), R(0,0,0,0,1), R(0,0,0,0,1) }},
    ['r' - ' '] = {{ R(0,0,0,0,0), R(0,0,0,0,0), R(1,0,1,1,0), R(1,1,0,0,0), R(1,0,0,0,0), R(1,0,0,0,0), R(1,0,0,0,0) }},
    ['s' - ' '] = {{ R(0,0,0,0,0), R(0,0,0,0,0), R(0,1,1,1,1), R(1,0,0,0,0), R(0,1,1,1,0), R(0,0,0,0,1), R(1,1,1,1,0) }},
    ['t' - ' '] = {{ R(0,1,0,0,0), R(0,1,0,0,0), R(1,1,1,0,0), R(0,1,0,0,0), R(0,1,0,0,0), R(0,1,0,0,0), R(0,0,1,1,0) }},
    ['u' - ' '] = {{ R(0,0,0,0,0), R(0,0,0,0,0), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(0,1,1,1,1) }},
    ['v' - ' '] = {{ R(0,0,0,0,0), R(0,0,0,0,0), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(0,1,0,1,0), R(0,0,1,0,0) }},
    ['w' - ' '] = {{ R(0,0,0,0,0), R(0,0,0,0,0), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,1,0,1), R(1,0,1,0,1), R(0,1,0,1,0) }},
    ['x' - ' '] = {{ R(0,0,0,0,0), R(0,0,0,0,0), R(1,0,0,0,1), R(0,1,0,1,0), R(0,0,1,0,0), R(0,1,0,1,0), R(1,0,0,0,1) }},
    ['y' - ' '] = {{ R(0,0,0,0,0), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(0,1,1,1,1), R(0,0,0,0,1), R(0,1,1,1,0) }},
    ['z' - ' '] = {{ R(0,0,0,0,0), R(0,0,0,0,0), R(1,1,1,1,1), R(0,0,0,1,0), R(0,0,1,0,0), R(0,1,0,0,0), R(1,1,1,1,1) }},

    ['?' - ' '] = {{ R(0,1,1,1,0), R(1,0,0,0,1), R(0,0,0,0,1), R(0,0,0,1,0), R(0,0,1,0,0), R(0,0,0,0,0), R(0,0,1,0,0) }},
};

#undef R

static const screen_glyph_t *glyph_for(char c)
{
    if (c < ' ' || c > 'z') {
        c = '?';
    }
    return &s_font[(unsigned char)c - (unsigned char)' '];
}

/* ------------------------------------------------------------------------- */
/* Tampon de trame — screen.c connait seul cette geometrie ; screen_anim.h    */
/* ne rend que des proportions, screen_view.h que du texte.                  */
/* ------------------------------------------------------------------------- */

static void fb_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

static void fb_set_pixel(int x, int y, bool on)
{
    /* Hors cadre : le glissement de bascule pousse volontairement du texte
     * hors-champ a droite en debut d'animation. Silencieusement ignore,
     * jamais une erreur — c'est le but recherche, pas un accident. */
    if (x < 0 || x >= (int)BOARD_OLED_WIDTH || y < 0 || y >= (int)BOARD_OLED_HEIGHT) {
        return;
    }
    const uint32_t idx = (uint32_t)(y / 8) * BOARD_OLED_WIDTH + (uint32_t)x;
    const uint8_t  bit = (uint8_t)(1u << (y % 8));
    if (on) {
        s_fb[idx] |= bit;
    } else {
        s_fb[idx] &= (uint8_t)~bit;
    }
}

/*
 * `ink` porte la couleur de l'encre, et sert au texte du bandeau inverse : le
 * bandeau est un pave blanc, son libelle s'y dessine donc en noir. Seuls les
 * pixels ALLUMES du glyphe sont ecrits — le fond du glyphe n'est jamais peint,
 * pour ne pas effacer ce qu'il y a autour (le pave du bandeau, ou le logo).
 */
static void draw_char(int x0, int y0, char c, bool ink)
{
    const screen_glyph_t *g = glyph_for(c);
    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 5; col++) {
            if (((g->rows[row] >> (4 - col)) & 1u) != 0) {
                fb_set_pixel(x0 + col, y0 + row, ink);
            }
        }
    }
}

static void draw_text(int x0, int y0, const char *s, bool ink)
{
    int x = x0;
    for (; *s != '\0'; s++) {
        draw_char(x, y0, *s, ink);
        x += (int)SCREEN_CHAR_PX;
    }
}

/*
 * Police double hauteur, par DOUBLEMENT DE PIXELS de la police 5x7 — un
 * glyphe de 10x14 dans une cellule de 12x16, sans table supplementaire.
 *
 * C'est le levier principal de l'affinage : Mae a decrit l'ecran precedent
 * comme « juste du texte en haut a gauche », « l'ecran est utilise 10% ». Une
 * seule information par ecran doit dominer, et la dominer veut dire l'ecrire
 * deux fois plus grand — pas la deplacer.
 *
 * Le doublement plutot qu'une vraie police 12x16 : une seconde table couterait
 * ~2 Kio de flash pour un gain a peine perceptible a cette taille sur une dalle
 * de 0,96 pouce. Si la grossierete se voit a l'oeil, on achetera la vraie
 * police — mais on ne paie pas d'avance.
 */
static void draw_char_2x(int x0, int y0, char c, bool ink)
{
    const screen_glyph_t *g = glyph_for(c);
    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 5; col++) {
            if (((g->rows[row] >> (4 - col)) & 1u) == 0) {
                continue;
            }
            /* Un pixel source devient un pave de 2x2. */
            for (int dy = 0; dy < 2; dy++) {
                for (int dx = 0; dx < 2; dx++) {
                    fb_set_pixel(x0 + col * 2 + dx, y0 + row * 2 + dy, ink);
                }
            }
        }
    }
}

static void draw_text_2x(int x0, int y0, const char *s, bool ink)
{
    int x = x0;
    for (; *s != '\0'; s++) {
        draw_char_2x(x, y0, *s, ink);
        x += (int)SCREEN_BIG_CHAR_PX;
    }
}

/* Centrage : l'origine vient de screen_center_x() (hmi/screen_layout.h, pur et
 * teste), pas d'une soustraction faite ici — c'est exactement le melange que
 * l'affinage devait defaire. */
static void draw_text_centered(int y0, const char *s, bool ink)
{
    draw_text((int)screen_center_x(BOARD_OLED_WIDTH, screen_text_px(s)), y0, s, ink);
}

static void draw_text_2x_centered(int y0, const char *s, bool ink)
{
    const uint16_t w = (uint16_t)(screen_text_px(s) * 2u);
    draw_text_2x((int)screen_center_x(BOARD_OLED_WIDTH, w), y0, s, ink);
}

/*
 * Bandeau inverse pleine largeur : un pave blanc de SCREEN_BAND_H de haut, son
 * libelle centre en noir.
 *
 * Sur un monochrome c'est l'element qui a le plus de poids visuel, et le seul
 * qui occupe les 128 px de large — ce que rien ne faisait avant. Il porte le
 * MODE et non l'etat : « au repos » ne dit rien, alors que savoir si la carte
 * OpenPGP est exposee est justement l'information qui compte.
 */
static void draw_band(int y0, const char *s)
{
    for (int y = 0; y < SCREEN_BAND_H; y++) {
        for (int x = 0; x < (int)BOARD_OLED_WIDTH; x++) {
            fb_set_pixel(x, y0 + y, true);
        }
    }
    /* +1 : le glyphe fait 7 lignes dans un bandeau de 10, centre a la ligne. */
    draw_text_centered(y0 + (SCREEN_BAND_H - 7) / 2, s, false);
}

/*
 * Les quatre points de cycle : lequel des modes est actif, et combien d'appuis
 * pour aller ailleurs.
 *
 * C'est le seul ajout de l'affinage qui apporte une information que Mae n'avait
 * pas. La LED donne une couleur qu'il faut memoriser — sa question « je suis en
 * bleu mais fait quoi ? » vient de la. Les points disent ou l'on est ET la
 * distance a parcourir.
 *
 * `active` vaut screen_mode_count() quand aucun mode connu n'est en cours :
 * aucun point ne se remplit, ce qui se distingue de « rien expose » (le
 * premier point plein).
 */
static void draw_dots(int y0, uint8_t active, uint8_t count)
{
    const int d     = 5;                       /* diametre d'un point */
    const int gap   = 7;
    const int total = count * d + (count - 1) * gap;
    int x = (int)screen_center_x(BOARD_OLED_WIDTH, (uint16_t)total);

    for (uint8_t i = 0; i < count; i++) {
        const bool full = (i == active);
        for (int yy = 0; yy < d; yy++) {
            for (int xx = 0; xx < d; xx++) {
                /* Disque approxime : on retire les quatre coins. */
                const bool corner = (xx == 0 || xx == d - 1) && (yy == 0 || yy == d - 1);
                if (corner) {
                    continue;
                }
                const bool edge = (xx == 0 || xx == d - 1 || yy == 0 || yy == d - 1);
                if (full || edge) {
                    fb_set_pixel(x + xx, y0 + yy, true);
                }
            }
        }
        x += d + gap;
    }
}

/* Segment epais, pour la coche et la croix. Parcours sur l'axe le plus long :
 * suffisant pour des diagonales courtes, et sans division. */
static void draw_line(int x0, int y0, int x1, int y1, int thick)
{
    const int dx = x1 - x0, dy = y1 - y0;
    const int adx = dx < 0 ? -dx : dx;
    const int ady = dy < 0 ? -dy : dy;
    const int steps = adx > ady ? adx : ady;
    if (steps == 0) {
        return;
    }
    for (int i = 0; i <= steps; i++) {
        const int x = x0 + (dx * i) / steps;
        const int y = y0 + (dy * i) / steps;
        for (int t = 0; t < thick; t++) {
            fb_set_pixel(x, y + t, true);
            fb_set_pixel(x + t, y, true);
        }
    }
}

/*
 * Coche et croix, tracees plutot que stockees : deux bitmaps de 32x32
 * couteraient 256 octets et figeraient l'epaisseur, alors que quatre segments
 * ne coutent rien et se reglent. Se lisent sans lire, ce qui est le but — un
 * verdict doit s'attraper d'un coup d'oeil.
 */
static void draw_check(int x0, int y0, int size)
{
    const int t = 3;
    draw_line(x0, y0 + size / 2, x0 + size / 3, y0 + size - t, t);
    draw_line(x0 + size / 3, y0 + size - t, x0 + size, y0, t);
}

static void draw_cross(int x0, int y0, int size)
{
    const int t = 3;
    draw_line(x0, y0, x0 + size, y0 + size, t);
    draw_line(x0 + size, y0, x0, y0 + size, t);
}

/* Barre de decompte : cadre 1 px, remplissage proportionnel a `permille`
 * (screen_bar_permille(), hmi/screen_anim.h) a l'interieur. */
static void draw_bar(int x0, int y0, int w, int h, uint16_t permille)
{
    for (int x = 0; x < w; x++) {
        fb_set_pixel(x0 + x, y0, true);
        fb_set_pixel(x0 + x, y0 + h - 1, true);
    }
    for (int y = 0; y < h; y++) {
        fb_set_pixel(x0, y0 + y, true);
        fb_set_pixel(x0 + w - 1, y0 + y, true);
    }
    const int inner_w = w - 2;
    const int filled  = (int)(((uint32_t)inner_w * permille) / 1000u);
    for (int x = 0; x < filled; x++) {
        for (int y = 1; y < h - 1; y++) {
            fb_set_pixel(x0 + 1 + x, y0 + y, true);
        }
    }
}

/*
 * Seule fonction qui melange l'instantane, les trois en-tetes purs et la
 * geometrie de l'ecran — c'est le "ou poser les pixels" que le brief laisse
 * a ce fichier, rien de plus. Choix de disposition faits ici, a revoir a
 * l'oeil : titre en haut, libelle dessous, barre de decompte en bas
 * (SCREEN_WAIT seulement), decalage anti-marquage applique en Y au texte,
 * glissement d'entree applique en X au titre (SCREEN_SWITCH seulement,
 * ancre sur l'instant ou hmi.c a arme l'evenement — event_at_ms).
 */
/* Defini plus bas, avec le format du bitmap genere. */
static void draw_logo(int x0, int y0);

static void render_frame(const hmi_snapshot_t *snap, uint32_t now)
{
    const screen_view_t v =
        screen_view_of(snap->mode, snap->confirm_pending, snap->op, snap->event);

    fb_clear();

    /* Decalage anti-marquage applique au CONTENU seulement, jamais aux
     * bandeaux : un bandeau colle en bas de la dalle et decale de 4 px sortirait
     * du cadre. Les bandeaux ne restent d'ailleurs jamais assez longtemps a
     * l'identique pour marquer — c'est l'ecran de repos qui le ferait, et lui
     * est decale. */
    const int shift = (int)screen_shift_px(now);

    switch (v.kind) {

    /* Confirmation : la seule information dont la lecture a une consequence
     * (quelle operation on autorise) en double hauteur, la barre en pleine
     * largeur, et les secondes en chiffre sous elle. La barre dit qu'il reste du
     * temps, le chiffre dit combien — deux generations de cles ont ete perdues
     * sur des expirations invisibles. */
    case SCREEN_WAIT: {
        draw_band(0, v.title);
        draw_text_2x_centered(20 + shift, screen_op_short(snap->op), true);

        const uint16_t pm = screen_bar_permille(snap->armed_at_ms, now);
        draw_bar(4, 42, (int)BOARD_OLED_WIDTH - 8, 8, pm);

        char secs[8];
        const uint16_t s = screen_seconds_left(snap->armed_at_ms, now);
        secs[0] = (char)('0' + (s / 10u) % 10u);
        secs[1] = (char)('0' + s % 10u);
        secs[2] = ' ';
        secs[3] = 's';
        secs[4] = '\0';
        draw_text_centered(54, (s >= 10u) ? secs : &secs[1], true);
        break;
    }

    /* Verdict : un glyphe qui se lit sans lire, et le mot dans le bandeau. */
    case SCREEN_VERDICT: {
        const int size = 30;
        const int x0   = (int)screen_center_x(BOARD_OLED_WIDTH, (uint16_t)size);
        if (screen_verdict_glyph(snap->event) == SCREEN_VERDICT_CHECK) {
            draw_check(x0, 6 + shift, size);
        } else {
            draw_cross(x0, 6 + shift, size);
        }
        draw_band((int)BOARD_OLED_HEIGHT - SCREEN_BAND_H, v.title);
        break;
    }

    /* Bascule de mode : le nom du nouveau mode en grand, glissant depuis la
     * droite, et les points qui disent la distance restante dans le cycle. */
    case SCREEN_SWITCH: {
        const uint16_t pm     = screen_slide_permille(snap->event_at_ms, now);
        const uint16_t w      = (uint16_t)(screen_text_px(v.title) * 2u);
        const int      target = (int)screen_center_x(BOARD_OLED_WIDTH, w);
        const int      x      = target
                              + (int)(((uint32_t)(1000u - pm) * BOARD_OLED_WIDTH) / 1000u);

        draw_text(2, 2, "MODE", true);
        draw_text_2x(x, 22 + shift, v.title, true);
        draw_dots(48, screen_mode_index(snap->mode), screen_mode_count());
        break;
    }

    /* Repos. Deux visages, et la frontiere est la seule decision de disposition
     * que ce fichier prend seul :
     *
     * - rien d'expose : le logo Niphargus occupe la dalle. C'est le choix de
     *   Mae (« au repos met le logo niphar plutot »), et il dit vrai — quand il
     *   n'y a rien a annoncer, l'ecran montre l'identite de la cle.
     * - un mode actif : le nom du mode en grand plus les points. Savoir que la
     *   carte OpenPGP est exposee n'est PAS du repos, et le taire derriere un
     *   logo serait taire la seule information qui compte a cet instant.
     */
    case SCREEN_IDLE:
    default:
        if (snap->mode == USB_MODE_NONE) {
            /* Pas de decalage ici, et c'est delibere : le logo plein fait
             * SCREEN_LOGO_HEIGHT = 64 px sur une dalle de 64 px, donc il n'a
             * AUCUNE marge verticale. Lui appliquer le decalage anti-marquage
             * ne le repartissait pas, ca lui rognait jusqu'a 4 lignes du bas
             * quatre minutes sur cinq — releve par la revue de branche. Cet
             * ecran ne dure de toute facon qu'une minute avant que la veille
             * errante prenne la main, et c'est elle qui porte l'anti-marquage. */
            const int x0 = (int)screen_center_x(BOARD_OLED_WIDTH, SCREEN_LOGO_WIDTH);
            draw_logo(x0, 0);
        } else {
            draw_text_2x_centered(14 + shift, v.title, true);
            draw_dots(46, screen_mode_index(snap->mode), screen_mode_count());
        }
        break;
    }
}

/* Logo Niphargus, 64x64, en hmi/screen_logo.h (genere par
 * tools/svg2bitmap.py depuis assets/nephar_logo_y.svg). MSB en tete de
 * chaque octet, ligne par ligne — voir le commentaire de tete du fichier
 * genere pour le format exact. */
static void draw_logo(int x0, int y0)
{
    for (int y = 0; y < (int)SCREEN_LOGO_HEIGHT; y++) {
        for (int x = 0; x < (int)SCREEN_LOGO_WIDTH; x++) {
            const uint32_t byte_idx = (uint32_t)y * (SCREEN_LOGO_WIDTH / 8u) + (uint32_t)x / 8u;
            const bool on = (screen_logo_bits[byte_idx] & (0x80u >> (x % 8))) != 0;
            fb_set_pixel(x0 + x, y0 + y, on);
        }
    }
}

/*
 * Ecran d'accueil : logo a gauche (pleine hauteur de l'ecran), nom et version
 * a droite. Duree et bascule vers l'ecran normal decidees par
 * screen_splash_active() (hmi/screen_anim.h, pur, teste sur l'hote) — rien
 * de decidable ici, juste la disposition des pixels, comme render_frame().
 * N'affiche ni mode ni instantane IHM : au tout premier appel de la tache,
 * rien de tout cela n'est encore une information a montrer.
 */
/*
 * Le meme logo a la moitie de l'echelle : 32x32 au lieu de 64x64, par
 * REDUCTION OR de blocs de 2x2 — un pave de quatre pixels source donne un pixel
 * allume si l'un des quatre l'etait.
 *
 * L'union plutot que la majorite : sur une silhouette de trait fin, exiger deux
 * pixels sur quatre effacerait les traits d'un pixel de large et troueraient le
 * dessin. L'union epaissit legerement, ce qui est le bon sens de l'erreur a
 * cette taille.
 *
 * Deux usages, tous deux nes de remarques de Mae : l'ecran d'accueil, ou le logo
 * pleine taille chevauchait le texte (« le splash ne correspond pas du tout a ce
 * tu m'avais montre, le texte chevauche le logo »), et la veille, ou il se
 * promene.
 */
static void draw_logo_half(int x0, int y0)
{
    for (int y = 0; y < (int)SCREEN_LOGO_HEIGHT / 2; y++) {
        for (int x = 0; x < (int)SCREEN_LOGO_WIDTH / 2; x++) {
            bool on = false;
            for (int dy = 0; dy < 2 && !on; dy++) {
                for (int dx = 0; dx < 2 && !on; dx++) {
                    const int sx = x * 2 + dx, sy = y * 2 + dy;
                    const uint32_t idx =
                        (uint32_t)sy * (SCREEN_LOGO_WIDTH / 8u) + (uint32_t)sx / 8u;
                    on = (screen_logo_bits[idx] & (0x80u >> (sx % 8))) != 0;
                }
            }
            if (on) {
                fb_set_pixel(x0 + x, y0 + y, true);
            }
        }
    }
}

#define SCREEN_LOGO_HALF_W ((int)SCREEN_LOGO_WIDTH / 2)
#define SCREEN_LOGO_HALF_H ((int)SCREEN_LOGO_HEIGHT / 2)

static void render_splash(void)
{
    fb_clear();

    /*
     * Logo a demi-echelle en haut, nom et version centres en dessous.
     *
     * Deux defauts corriges ici, dans l'ordre ou ils ont ete trouves :
     *
     * 1. Le texte etait pose a x = SCREEN_LOGO_WIDTH + 4, soit 68, ou il ne
     *    restait que 60 px — dix caracteres. Or il n'y a AUCUN tag dans ce
     *    depot : « git describe --tags --always --dirty » rend un hash de treize
     *    caracteres, et la version etait donc coupee. Sans risque memoire
     *    (fb_set_pixel() borne les quatre cotes) mais un affichage tronque est
     *    un affichage faux.
     * 2. Ma premiere correction remontait le logo pleine taille de 8 px : il
     *    occupait encore y=0..55 et CHEVAUCHAIT le texte pose a y=47. Mae l'a vu
     *    tout de suite. A demi-echelle le logo tient dans 32 lignes et il reste
     *    de la place franche.
     *
     * En pleine largeur, vingt-un caracteres passent : de quoi tenir un
     * « v0.3.1-12-gdeadbee » entier le jour ou un tag existera.
     */
    draw_logo_half((int)screen_center_x(BOARD_OLED_WIDTH, SCREEN_LOGO_HALF_W), 2);
    draw_text_centered(SCREEN_LOGO_HALF_H + 8,  "NIPHARGUS",     true);
    draw_text_centered(SCREEN_LOGO_HALF_H + 19, NIPHAR_VERSION, true);
}

/*
 * Veille : le logo se PROMENE, au lieu que la dalle s'eteigne.
 *
 * Mae voulait le logo en veille (« en veille sa serai cool d'avoir le logo en
 * waiting ») plutot qu'un ecran noir. Un logo fixe se graverait dans la dalle ;
 * a demi-echelle et en mouvement, il couvre 1024 pixels sur 8192 et chaque pixel
 * n'est donc allume qu'une fraction du temps. On y gagne des deux cotes.
 *
 * Le mode voyage AVEC le logo quand quelque chose est expose : le taire pendant
 * la veille tairait la seule information dont l'absence peut surprendre — qu'une
 * carte OpenPGP est branchee. Et il voyage plutot que d'etre fixe en bas, sinon
 * c'est lui qui marquerait la dalle.
 */
static void render_standby(usb_mode_t mode, uint32_t now)
{
    fb_clear();

    const char *label   = (mode == USB_MODE_NONE) ? NULL : screen_mode_name(mode);
    const int   label_w = (label != NULL) ? (int)screen_text_px(label) : 0;
    const int   group_w = (label_w > SCREEN_LOGO_HALF_W) ? label_w : SCREEN_LOGO_HALF_W;
    const int   group_h = SCREEN_LOGO_HALF_H + ((label != NULL) ? 10 : 0);

    /* screen_span() et non une soustraction a la main : si le groupe depassait
     * l'ecran, le non signe se replierait sur des dizaines de milliers et le
     * logo partirait hors champ — defaisant la fonction anti-marquage pour
     * laquelle cette veille existe. Releve par la revue de branche. */
    const uint16_t span_x = screen_span(BOARD_OLED_WIDTH,  (uint16_t)group_w);
    const uint16_t span_y = screen_span(BOARD_OLED_HEIGHT, (uint16_t)group_h);
    const int x = (int)screen_wander(now, SCREEN_WANDER_X_MS, span_x);
    const int y = (int)screen_wander(now, SCREEN_WANDER_Y_MS, span_y);

    draw_logo_half(x + (group_w - SCREEN_LOGO_HALF_W) / 2, y);
    if (label != NULL) {
        draw_text(x + (group_w - label_w) / 2, y + SCREEN_LOGO_HALF_H + 2, label, true);
    }
}

/* ------------------------------------------------------------------------- */
/* Pilote SSD1306.                                                            */
/* ------------------------------------------------------------------------- */

static esp_err_t i2c_bus_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = -1,
        .sda_io_num        = BOARD_OLED_SDA,
        .scl_io_num        = BOARD_OLED_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            /*
             * FALSE : deux pull-ups externes de 4,7 kOhm sont posees sur le
             * module (voir boards/wt9932_key/board.h). Un sondage anterieur
             * a leur pose avait active les internes faute de mieux ; les
             * activer ici decrirait un montage qui n'existe plus.
             */
            .enable_internal_pullup = false,
        },
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        return err;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BOARD_OLED_ADDR,
        .scl_speed_hz    = 400000,
    };
    return i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
}

/*
 * Sequence eprouvee sur module reel le 2026-08-17 (sondage jetable, carte
 * WT9932P4-TINY) : display off, adressage horizontal, remap segment, scan
 * inverse, charge pump, affichage normal, display on. Prefixee du controle
 * 0x00 (flux de commandes). Ne pas "completer" avec d'autres commandes
 * (multiplex, COM pins, contraste...) sans un nouveau sondage — celle-ci
 * suffit deja a dessiner un cadre complet sur les quatre bords.
 */
static esp_err_t ssd1306_init_sequence(void)
{
    static const uint8_t seq[] = {
        0x00,
        0xAE,             /* display off */
        0x20, 0x00,       /* adressage horizontal */
        0xA1,             /* remap segment */
        0xC8,             /* scan (COM) inverse */
        0x8D, 0x14,       /* charge pump on */
        0xA6,             /* affichage normal (pas inverse) */
        0xAF,             /* display on */
    };
    return i2c_master_transmit(s_dev, seq, sizeof(seq), SCREEN_I2C_TIMEOUT_MS);
}

static esp_err_t ssd1306_flush(void)
{
    for (unsigned page = 0; page < SCREEN_PAGES; page++) {
        const uint8_t pos[4] = { 0x00, (uint8_t)(0xB0 | page), 0x00, 0x10 };
        esp_err_t err = i2c_master_transmit(s_dev, pos, sizeof(pos), SCREEN_I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }

        uint8_t data[1 + BOARD_OLED_WIDTH];
        data[0] = 0x40;
        memcpy(&data[1], &s_fb[page * BOARD_OLED_WIDTH], BOARD_OLED_WIDTH);
        err = i2c_master_transmit(s_dev, data, sizeof(data), SCREEN_I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

/* Allumage/extinction de la dalle. 0xAE/0xAF ne touchent pas la RAM d'affichage :
 * rallumer reaffiche ce qui y etait, donc rien a redessiner. */
static esp_err_t ssd1306_display(bool on)
{
    const uint8_t cmd[2] = { 0x00, on ? 0xAFu : 0xAEu };
    return i2c_master_transmit(s_dev, cmd, sizeof(cmd), SCREEN_I2C_TIMEOUT_MS);
}

/*
 * « Quelque chose a-t-il change ? » — la definition d'activite pour
 * l'anti-remanence.
 *
 * Volontairement fonde sur l'instantane IHM et non sur un horodatage que hmi.c
 * aurait a publier : ce qui use la dalle, c'est un contenu IDENTIQUE affiche en
 * continu, et l'instantane est exactement ce qui determine le contenu. Aucune
 * modification de hmi.{c,h} n'a donc ete necessaire.
 *
 * Comparaison champ par champ et non memcmp() : le bourrage d'une structure
 * n'est pas initialise de facon garantie, et un memcmp() verrait des
 * changements imaginaires — l'ecran ne s'eteindrait jamais.
 */
static bool snap_differs(const hmi_snapshot_t *a, const hmi_snapshot_t *b)
{
    return a->mode            != b->mode
        || a->confirm_pending != b->confirm_pending
        || a->armed_at_ms     != b->armed_at_ms
        || a->op              != b->op
        || a->event           != b->event
        || a->event_at_ms     != b->event_at_ms;
}

static void screen_task(void *arg)
{
    (void)arg;
    /* Ancre de l'ecran d'accueil : le demarrage de CETTE tache, pas la mise
     * sous tension — screen_init() est deja appele apres hmi_init() (voir
     * main.c), donc "au demarrage de la tache d'affichage" est la seule
     * definition que ce fichier puisse honorer sans connaitre l'instant de
     * boot. */
    const uint32_t splash_start_ms = (uint32_t)(esp_timer_get_time() / 1000);

    hmi_snapshot_t prev = {0};
    uint32_t       last_activity_ms = splash_start_ms;
    bool           display_on       = true;
    unsigned       fails            = 0;
    bool           hwm_reported     = false;

    for (;;) {
        hmi_snapshot_t snap;
        hmi_snapshot(&snap);

        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        const bool     splash = screen_splash_active(splash_start_ms, now);

        if (snap_differs(&snap, &prev)) {
            prev             = snap;
            last_activity_ms = now;
        }

        /*
         * Veille anti-remanence. Un OLED qui affiche le meme contenu en continu
         * le garde en trace permanente.
         *
         * La veille PROMENE le logo au lieu d'eteindre : c'est ce que Mae voulait
         * voir, et c'est aussi une meilleure protection qu'un logo fixe. Mais elle
         * ne suffit pas seule pour une cle branchee des annees, d'ou l'extinction
         * franche au bout de SCREEN_SLEEP_AFTER_MS.
         *
         * Jamais de veille pendant l'accueil (la dalle vient de s'allumer), ni
         * tant qu'une confirmation attend : eteindre — ou meme animer — l'ecran
         * sous le nez de quelqu'un a qui on demande d'appuyer serait exactement
         * le contraire du but de cet ecran.
         */
        const bool busy     = splash || snap.confirm_pending;
        const bool standby  = !busy && screen_blank_after_ms(last_activity_ms, now);
        const bool want_on  = busy || !screen_sleep_after_ms(last_activity_ms, now);

        if (want_on != display_on) {
            const esp_err_t derr = ssd1306_display(want_on);
            if (derr == ESP_OK) {
                display_on = want_on;
            } else {
                ESP_LOGW(TAG, "bascule d'alimentation de la dalle : %s",
                         esp_err_to_name(derr));
            }
        }

        /* Dalle eteinte : rien a dessiner et rien a transmettre. Le bus se
         * tait, ce qui est aussi la seule facon de ne rien lui couter au repos. */
        if (!display_on) {
            vTaskDelay(pdMS_TO_TICKS(SCREEN_TASK_MS));
            continue;
        }

        if (splash) {
            render_splash();
        } else if (standby) {
            render_standby(snap.mode, now);
        } else {
            render_frame(&snap, now);
        }

        /* Erreur I2C : on journalise et on saute cette image, jamais de
         * blocage — la tache IHM ne doit jamais attendre apres celle-ci. */
        const esp_err_t err = ssd1306_flush();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "trame sautee : %s", esp_err_to_name(err));
            fails++;
        } else {
            fails = 0;
        }

        /*
         * Echecs repetes : rejouer la sequence d'initialisation.
         *
         * Retransmettre indefiniment la meme trame ne repare qu'une perte
         * transitoire. Si le SSD1306 a perdu son etat interne — et le sondage
         * materiel a montre que ce bus est electriquement marginal, avec des
         * adresses parasites persistant meme apres la pose des pull-ups — alors
         * seule la sequence d'init le remet d'aplomb.
         *
         * Le pire cas que ca evite est precis : on demande a quelqu'un
         * d'appuyer pour autoriser une signature, et l'ecran est noir. La LED
         * porte encore l'information, mais l'ecran est ce qui dit QUOI on
         * autorise.
         */
        if (fails >= SCREEN_REINIT_AFTER_FAILS) {
            ESP_LOGW(TAG, "%u trames perdues d'affilee — reinitialisation du SSD1306", fails);
            const esp_err_t rerr = ssd1306_init_sequence();
            if (rerr == ESP_OK) {
                /* La sequence rallume la dalle (0xAF) : l'etat suivi doit le
                 * refleter, sinon la prochaine comparaison want_on/display_on
                 * croirait n'avoir rien a faire. */
                display_on = true;
                ESP_LOGI(TAG, "SSD1306 reinitialise");
            } else {
                ESP_LOGE(TAG, "reinitialisation echouee : %s", esp_err_to_name(rerr));
            }
            fails = 0;
        }

        /* Marge de pile, mesuree et non estimee, une fois par demarrage. La
         * revue de branche a releve que l'estimation de la tache 4 etait
         * perimee : le code a grossi depuis (police double hauteur, logo,
         * points, veille errante). Une ligne au journal repond a la question a
         * chaque boot, sur le vrai materiel, plutot qu'au desassemblage. */
        if (!hwm_reported && (uint32_t)(now - splash_start_ms) >= SCREEN_HWM_REPORT_MS) {
            hwm_reported = true;
            ESP_LOGI(TAG, "marge de pile : %u octets libres sur %u",
                     (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
                     (unsigned)SCREEN_TASK_STACK);
        }

        vTaskDelay(pdMS_TO_TICKS(SCREEN_TASK_MS));
    }
}

esp_err_t screen_init(void)
{
    esp_err_t err = i2c_bus_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bus I2C indisponible : %s — la cle reste utilisable sans ecran",
                 esp_err_to_name(err));
        return err;
    }

    err = ssd1306_init_sequence();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init SSD1306 : %s — la cle reste utilisable sans ecran",
                 esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(screen_task, "screen", SCREEN_TASK_STACK, NULL, SCREEN_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "creation de la tache ecran impossible");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ecran SSD1306 pret, SCL=IO%d SDA=IO%d adresse 0x%02X",
             BOARD_OLED_SCL, BOARD_OLED_SDA, BOARD_OLED_ADDR);
    return ESP_OK;
}

#else /* pas d'ecran sur cette carte */

esp_err_t screen_init(void)
{
    return ESP_OK;   /* no-op : main.c n'a pas a savoir quelle carte tourne */
}

#endif
