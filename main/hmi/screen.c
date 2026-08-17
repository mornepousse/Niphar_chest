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

static void draw_char(int x0, int y0, char c)
{
    const screen_glyph_t *g = glyph_for(c);
    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 5; col++) {
            const bool on = ((g->rows[row] >> (4 - col)) & 1u) != 0;
            fb_set_pixel(x0 + col, y0 + row, on);
        }
    }
}

static void draw_text(int x0, int y0, const char *s)
{
    int x = x0;
    for (; *s != '\0'; s++) {
        draw_char(x, y0, *s);
        x += 6;   /* 5 colonnes de glyphe + 1 de marge */
    }
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
static void render_frame(const hmi_snapshot_t *snap, uint32_t now)
{
    const screen_view_t v =
        screen_view_of(snap->mode, snap->confirm_pending, snap->op, snap->event);

    fb_clear();

    const uint8_t shift_y = screen_shift_px(now);

    int title_x = 2;
    if (v.kind == SCREEN_SWITCH) {
        const uint16_t pm     = screen_slide_permille(snap->event_at_ms, now);
        const uint32_t travel = BOARD_OLED_WIDTH;   /* hors-champ a droite au depart */
        title_x += (int)(((uint32_t)(1000u - pm) * travel) / 1000u);
    }

    draw_text(title_x, 4 + shift_y, v.title);
    if (v.line[0] != '\0') {
        draw_text(2, 24 + shift_y, v.line);
    }

    if (v.kind == SCREEN_WAIT) {
        const uint16_t pm = screen_bar_permille(snap->armed_at_ms, now);
        draw_bar(4, 52, (int)BOARD_OLED_WIDTH - 8, 8, pm);
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

static void screen_task(void *arg)
{
    (void)arg;
    for (;;) {
        hmi_snapshot_t snap;
        hmi_snapshot(&snap);

        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        render_frame(&snap, now);

        /* Erreur I2C : on journalise et on saute cette image, jamais de
         * blocage — la tache IHM ne doit jamais attendre apres celle-ci. */
        const esp_err_t err = ssd1306_flush();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "trame sautee : %s", esp_err_to_name(err));
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
