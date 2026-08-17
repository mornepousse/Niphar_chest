/* La barre de decompte est la seule animation qui n'est pas decorative : elle
 * rend visibles les 15 s de SEC_CONFIRM_TIMEOUT_MS, aujourd'hui muettes. Deux
 * generations de cles ont echoue le 2026-08-17 sur des expirations que rien
 * n'annoncait. */
#include "test_framework.h"

#include "sec_confirm.h"
#include "hmi/screen_anim.h"

static void test_bar_is_full_at_arming(void)
{
    TEST_ASSERT_EQ(screen_bar_permille(1000, 1000), 1000, "pleine a l'armement");
}

static void test_bar_is_empty_at_deadline(void)
{
    TEST_ASSERT_EQ(screen_bar_permille(1000, 1000 + SEC_CONFIRM_TIMEOUT_MS), 0,
                   "vide a l'echeance");
}

static void test_bar_is_half_at_half_time(void)
{
    const uint32_t half = SEC_CONFIRM_TIMEOUT_MS / 2u;
    const uint16_t p = screen_bar_permille(1000, 1000 + half);
    TEST_ASSERT(p > 480 && p < 520, "environ la moitie a mi-parcours");
}

/* Jamais hors bornes : une barre negative ou au-dela de 100 % se dessine
 * n'importe ou et masque le reste de l'ecran. */
static void test_bar_never_leaves_its_bounds(void)
{
    for (uint32_t d = 0; d <= SEC_CONFIRM_TIMEOUT_MS + 5000u; d += 250u) {
        const uint16_t p = screen_bar_permille(1000, 1000 + d);
        TEST_ASSERT(p <= 1000, "jamais au-dela du plein");
    }
}

/* Le compteur d'ESP-IDF repasse par zero apres ~49 jours ; une cle peut rester
 * branchee plus longtemps. */
static void test_bar_survives_millisecond_wraparound(void)
{
    const uint32_t armed = 0xFFFFF000u;   /* non aligne sur rien */
    TEST_ASSERT_EQ(screen_bar_permille(armed, armed), 1000, "pleine a l'armement");
    TEST_ASSERT_EQ(screen_bar_permille(armed, armed + SEC_CONFIRM_TIMEOUT_MS), 0,
                   "vide a l'echeance, a cheval sur le repassage a zero");
    const uint16_t p = screen_bar_permille(armed, armed + SEC_CONFIRM_TIMEOUT_MS / 2u);
    TEST_ASSERT(p > 480 && p < 520, "et juste au milieu");
}

/* Le decalage anti-marquage ne doit jamais pousser le contenu hors de l'ecran. */
static void test_shift_stays_within_bounds(void)
{
    for (uint32_t t = 0; t < 3600u * 1000u; t += 7919u) {
        const uint8_t s = screen_shift_px(t);
        TEST_ASSERT(s <= SCREEN_SHIFT_MAX_PX, "le decalage reste borne");
    }
}

/* Il doit bouger, sinon il ne sert a rien. */
static void test_shift_actually_moves(void)
{
    bool seen_other = false;
    const uint8_t first = screen_shift_px(0);
    for (uint32_t t = 0; t < 3600u * 1000u; t += 60u * 1000u) {
        if (screen_shift_px(t) != first) { seen_other = true; break; }
    }
    TEST_ASSERT(seen_other, "le decalage change au fil du temps");
}

static void test_slide_runs_from_zero_to_full(void)
{
    TEST_ASSERT_EQ(screen_slide_permille(500, 500), 0, "commence a zero");
    TEST_ASSERT_EQ(screen_slide_permille(500, 500 + SCREEN_SLIDE_MS), 1000, "finit au plein");
    for (uint32_t d = 0; d <= SCREEN_SLIDE_MS + 2000u; d += 25u) {
        TEST_ASSERT(screen_slide_permille(500, 500 + d) <= 1000, "jamais au-dela");
    }
}

void test_screen_anim(void)
{
    TEST_SUITE("screen_anim");
    TEST_RUN(test_bar_is_full_at_arming);
    TEST_RUN(test_bar_is_empty_at_deadline);
    TEST_RUN(test_bar_is_half_at_half_time);
    TEST_RUN(test_bar_never_leaves_its_bounds);
    TEST_RUN(test_bar_survives_millisecond_wraparound);
    TEST_RUN(test_shift_stays_within_bounds);
    TEST_RUN(test_shift_actually_moves);
    TEST_RUN(test_slide_runs_from_zero_to_full);
}
