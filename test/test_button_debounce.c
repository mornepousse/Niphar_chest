/* L'anti-rebond est la seule chose entre un contact mecanique et une decision
 * de securite. Un rebond pris pour un appui, c'est une signature accordee que
 * personne n'a demandee. */
#include "test_framework.h"

#include "hmi/button_debounce.h"

static void test_quiet_line_emits_nothing(void)
{
    btn_debounce_t b;
    btn_init(&b);
    for (uint32_t t = 0; t < 500; t += 5) {
        TEST_ASSERT_EQ(btn_feed(&b, false, t), BTN_NONE, "ligne au repos, aucun front");
    }
}

/* Un appui franc doit produire UN front, apres le delai, et pas avant. */
static void test_clean_press_emits_one_event(void)
{
    btn_debounce_t b;
    btn_init(&b);
    btn_feed(&b, false, 0);
    TEST_ASSERT_EQ(btn_feed(&b, true, 100), BTN_NONE, "rien avant le delai");
    TEST_ASSERT_EQ(btn_feed(&b, true, 100 + BTN_DEBOUNCE_MS - 1), BTN_NONE,
                   "toujours rien une milliseconde trop tot");
    TEST_ASSERT_EQ(btn_feed(&b, true, 100 + BTN_DEBOUNCE_MS), BTN_PRESSED,
                   "le front sort pile au delai");
}

/* Le cas qui compte : maintenir le bouton ne doit produire qu'UN seul front,
 * jamais une repetition. Sinon un doigt pose signerait en boucle. */
static void test_long_hold_emits_exactly_one(void)
{
    btn_debounce_t b;
    btn_init(&b);
    btn_feed(&b, false, 0);
    int events = 0;
    for (uint32_t t = 10; t < 5000; t += 5) {
        if (btn_feed(&b, true, t) == BTN_PRESSED) events++;
    }
    TEST_ASSERT_EQ(events, 1, "un maintien de 5 s ne produit qu'un seul front");
}

/* Rebond au moment du contact : la ligne oscille, un seul front doit sortir. */
static void test_bouncing_contact_emits_one(void)
{
    btn_debounce_t b;
    btn_init(&b);
    btn_feed(&b, false, 0);
    const bool bounce[] = { true, false, true, false, true, true };
    int events = 0;
    uint32_t t = 10;
    for (unsigned i = 0; i < sizeof(bounce) / sizeof(bounce[0]); i++, t += 3) {
        if (btn_feed(&b, bounce[i], t) != BTN_NONE) events++;
    }
    TEST_ASSERT_EQ(events, 0, "pendant le rebond, aucun front n'est encore stable");
    TEST_ASSERT_EQ(btn_feed(&b, true, t + BTN_DEBOUNCE_MS), BTN_PRESSED,
                   "le front sort une fois la ligne calmee");
}

/* Un rebond PENDANT le maintien ne doit pas produire de relachement fantome. */
static void test_bounce_during_hold_emits_no_release(void)
{
    btn_debounce_t b;
    btn_init(&b);
    btn_feed(&b, false, 0);
    btn_feed(&b, true, 10);
    TEST_ASSERT_EQ(btn_feed(&b, true, 10 + BTN_DEBOUNCE_MS), BTN_PRESSED, "appui etabli");
    /* micro-coupure de 3 ms, bien plus courte que le delai */
    TEST_ASSERT_EQ(btn_feed(&b, false, 200), BTN_NONE, "la coupure ne compte pas");
    TEST_ASSERT_EQ(btn_feed(&b, true, 203), BTN_NONE, "ni son retour");
    TEST_ASSERT_EQ(btn_feed(&b, true, 300), BTN_NONE, "et rien ne sort apres");
}

static void test_release_emits_release(void)
{
    btn_debounce_t b;
    btn_init(&b);
    btn_feed(&b, false, 0);
    btn_feed(&b, true, 10);
    btn_feed(&b, true, 10 + BTN_DEBOUNCE_MS);
    TEST_ASSERT_EQ(btn_feed(&b, false, 500), BTN_NONE, "rien au relachement immediat");
    TEST_ASSERT_EQ(btn_feed(&b, false, 500 + BTN_DEBOUNCE_MS), BTN_RELEASED,
                   "le relachement sort apres le meme delai");
}

/* Le compteur de millisecondes d'ESP-IDF repasse par zero apres ~49 jours.
 * Une clé peut rester branchee plus longtemps que ca. */
static void test_survives_millisecond_wraparound(void)
{
    btn_debounce_t b;
    btn_init(&b);
    const uint32_t near_wrap = 0xFFFFFFF0u;
    btn_feed(&b, false, near_wrap);
    btn_feed(&b, true, near_wrap + 1);
    /* +25 ms depuis near_wrap+1, en passant par zero */
    TEST_ASSERT_EQ(btn_feed(&b, true, near_wrap + 1 + 25), BTN_PRESSED,
                   "le front sort meme a cheval sur le repassage a zero");
}

void test_button_debounce(void)
{
    TEST_SUITE("button_debounce");
    TEST_RUN(test_quiet_line_emits_nothing);
    TEST_RUN(test_clean_press_emits_one_event);
    TEST_RUN(test_long_hold_emits_exactly_one);
    TEST_RUN(test_bouncing_contact_emits_one);
    TEST_RUN(test_bounce_during_hold_emits_no_release);
    TEST_RUN(test_release_emits_release);
    TEST_RUN(test_survives_millisecond_wraparound);
}
