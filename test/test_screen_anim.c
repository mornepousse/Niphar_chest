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

/* Valeur exacte, pas une tolerance : 15000/2 = 7500, donc 7500*1000/15000 =
 * 500 pile. Une tolerance de +-2% laisse passer un denominateur legerement
 * faux (ex. SEC_CONFIRM_TIMEOUT_MS - 1 par coquille de refactor) — voir
 * test_bar_matches_exact_fractions ci-dessous, qui verifie deux autres
 * points exacts pour la meme raison. */
static void test_bar_is_half_at_half_time(void)
{
    const uint32_t half = SEC_CONFIRM_TIMEOUT_MS / 2u;
    TEST_ASSERT_EQ(screen_bar_permille(1000, 1000 + half), 500,
                   "exactement la moitie a mi-parcours");
}

/* Deux points exacts de plus, au quart et aux trois quarts du temps ecoule.
 * Avec un seul point intermediaire (le milieu), une erreur systematique sur
 * le denominateur (par ex. SEC_CONFIRM_TIMEOUT_MS - 1) peut rester invisible
 * si elle ne pousse jamais l'arrondi au-dela de l'entier attendu a CE point
 * precis ; plusieurs points exacts a des fractions differentes du parcours
 * reduisent cette marge. */
static void test_bar_matches_exact_fractions(void)
{
    const uint32_t quarter = SEC_CONFIRM_TIMEOUT_MS / 4u;
    const uint32_t three_quarters = (SEC_CONFIRM_TIMEOUT_MS * 3u) / 4u;
    TEST_ASSERT_EQ(screen_bar_permille(1000, 1000 + quarter), 750,
                   "trois quarts au quart du temps ecoule");
    TEST_ASSERT_EQ(screen_bar_permille(1000, 1000 + three_quarters), 250,
                   "un quart aux trois quarts du temps ecoule");
}

/* Les points "ronds" ci-dessus (moitie, quart, trois quarts) ne suffisent
 * PAS a attraper un denominateur legerement faux : verifie a la main sur les
 * 15001 valeurs entieres de elapsed, un denominateur SEC_CONFIRM_TIMEOUT_MS -
 * 1 ne diverge du bon calcul qu'a UNE seule milliseconde (elapsed=1, 999 au
 * lieu de 1000) — 14999 et 15000 sont si proches que la troncature entiere
 * absorbe l'erreur presque partout ailleurs, y compris a chaque fraction
 * ronde. Seul un balayage exhaustif contre la formule de reference l'attrape
 * de facon fiable. Cette formule est deliberement la meme que celle du header
 * : c'est la specification (le brief l'a fixee telle quelle), pas une copie
 * d'implementation — ce test n'attraperait pas une erreur commise
 * identiquement aux deux endroits, mais c'est le seul defaut qu'on lui
 * connaisse. */
static void test_bar_matches_reference_formula_exhaustively(void)
{
    for (uint32_t d = 0; d <= SEC_CONFIRM_TIMEOUT_MS; d++) {
        const uint32_t remaining = SEC_CONFIRM_TIMEOUT_MS - d;
        const uint16_t expected = (uint16_t)(((uint64_t)remaining * 1000u)
                                             / SEC_CONFIRM_TIMEOUT_MS);
        TEST_ASSERT_EQ(screen_bar_permille(1000, 1000 + d), expected,
                       "correspond a la formule de reference, milliseconde par milliseconde");
    }
}

/* Un decompte qui remonte, meme brievement, est pire qu'une barre absente :
 * il fait douter de ce qu'on lit, sur l'affichage dont tout l'interet est
 * d'etre cru. Rien dans les tests precedents n'empeche une implementation
 * qui oscille tant qu'elle touche les points echantillonnes et reste dans
 * les bornes. */
static void test_bar_is_monotonic_non_increasing(void)
{
    uint16_t prev = screen_bar_permille(1000, 1000);
    for (uint32_t d = 1; d <= SEC_CONFIRM_TIMEOUT_MS; d += 50u) {
        const uint16_t cur = screen_bar_permille(1000, 1000 + d);
        TEST_ASSERT(cur <= prev, "la barre ne remonte jamais");
        prev = cur;
    }
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

/* screen_slide_permille() partage l'idiome de soustraction non signee de
 * screen_bar_permille() (`now_ms - started_ms`), donc le meme repassage a
 * zero du compteur de millisecondes s'applique. */
static void test_slide_survives_millisecond_wraparound(void)
{
    const uint32_t started = 0xFFFFFFF0u;   /* a 16 ms du repassage a zero */
    TEST_ASSERT_EQ(screen_slide_permille(started, started), 0,
                   "commence a zero, a cheval sur le repassage a zero");
    TEST_ASSERT_EQ(screen_slide_permille(started, started + SCREEN_SLIDE_MS), 1000,
                   "finit au plein, a cheval sur le repassage a zero");
    TEST_ASSERT_EQ(screen_slide_permille(started, started + SCREEN_SLIDE_MS / 2u), 500,
                   "exactement la moitie, a cheval sur le repassage a zero");
}

void test_screen_anim(void)
{
    TEST_SUITE("screen_anim");
    TEST_RUN(test_bar_is_full_at_arming);
    TEST_RUN(test_bar_is_empty_at_deadline);
    TEST_RUN(test_bar_is_half_at_half_time);
    TEST_RUN(test_bar_matches_exact_fractions);
    TEST_RUN(test_bar_matches_reference_formula_exhaustively);
    TEST_RUN(test_bar_is_monotonic_non_increasing);
    TEST_RUN(test_bar_never_leaves_its_bounds);
    TEST_RUN(test_bar_survives_millisecond_wraparound);
    TEST_RUN(test_shift_stays_within_bounds);
    TEST_RUN(test_shift_actually_moves);
    TEST_RUN(test_slide_runs_from_zero_to_full);
    TEST_RUN(test_slide_survives_millisecond_wraparound);
}
