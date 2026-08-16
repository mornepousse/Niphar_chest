/* La LED est la seule chose qui dit a l'utilisateur QUAND appuyer. Une porte de
 * presence physique qu'on ne sait pas quand franchir ne protege rien : elle
 * fait juste rater des signatures. Ce mapping doit donc etre total et sans
 * ambiguite. */
#include "test_framework.h"

#include "usb/usb_mode.h"
#include "hmi/led_state.h"

static bool rgb_eq(led_rgb_t a, led_rgb_t b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

/* Totalite : aucun etat ne doit laisser la LED dans un etat indefini. */
static void test_every_mode_has_a_view(void)
{
    for (int m = 0; m <= USB_MODE_COUNT; m++) {
        led_view_t v = led_state_view((usb_mode_t)m, false, LED_EVENT_NONE);
        TEST_ASSERT(v.pattern == LED_PATTERN_OFF || v.pattern == LED_PATTERN_STEADY,
                    "chaque mode au repos donne un motif connu");
    }
}

/* L'etat muet doit etre visiblement muet : une cle eteinte n'expose rien. */
static void test_none_is_dark(void)
{
    led_view_t v = led_state_view(USB_MODE_NONE, false, LED_EVENT_NONE);
    TEST_ASSERT_EQ(v.pattern, LED_PATTERN_OFF, "l'etat muet eteint la LED");
    TEST_ASSERT(rgb_eq(v.rgb, (led_rgb_t){0, 0, 0}), "et ne laisse aucune couleur");
}

/* Deux modes de meme couleur seraient pires que pas de LED du tout : on
 * croirait signer avec PGP en etant en OTP. */
static void test_pgp_and_otp_differ(void)
{
    led_view_t p = led_state_view(USB_MODE_PGP, false, LED_EVENT_NONE);
    led_view_t o = led_state_view(USB_MODE_OTP, false, LED_EVENT_NONE);
    TEST_ASSERT(!rgb_eq(p.rgb, o.rgb), "pgp et otp n'ont pas la meme couleur");
    TEST_ASSERT(!rgb_eq(p.rgb, (led_rgb_t){0, 0, 0}), "pgp est visible");
    TEST_ASSERT(!rgb_eq(o.rgb, (led_rgb_t){0, 0, 0}), "otp est visible");

    /* storage n'etait eprouve par rien : la boucle de totalite ne regarde que
     * le motif, jamais la couleur. */
    led_view_t s = led_state_view(USB_MODE_STORAGE, false, LED_EVENT_NONE);
    TEST_ASSERT(!rgb_eq(s.rgb, p.rgb) && !rgb_eq(s.rgb, o.rgb),
                "storage ne partage sa couleur ni avec pgp ni avec otp");
    TEST_ASSERT(!rgb_eq(s.rgb, (led_rgb_t){0, 0, 0}),
                "storage est visible, pas confondu avec l'etat muet");
}

/* La pulsation ne doit signifier qu'une chose : « j'attends ton doigt ». */
static void test_pulse_means_waiting_and_nothing_else(void)
{
    TEST_ASSERT_EQ(led_state_view(USB_MODE_PGP, true, LED_EVENT_NONE).pattern,
                   LED_PATTERN_PULSE, "attente en pgp -> pulsation");
    TEST_ASSERT_EQ(led_state_view(USB_MODE_OTP, true, LED_EVENT_NONE).pattern,
                   LED_PATTERN_PULSE, "attente en otp -> pulsation");
    TEST_ASSERT_EQ(led_state_view(USB_MODE_PGP, false, LED_EVENT_NONE).pattern,
                   LED_PATTERN_STEADY, "hors attente, la LED est fixe");
}

/* La pulsation garde la couleur du mode : sinon on perd l'info du mode au
 * moment precis ou on demande a l'utilisateur d'autoriser quelque chose. */
static void test_pulse_keeps_the_mode_colour(void)
{
    led_view_t idle = led_state_view(USB_MODE_PGP, false, LED_EVENT_NONE);
    led_view_t wait = led_state_view(USB_MODE_PGP, true,  LED_EVENT_NONE);
    TEST_ASSERT(rgb_eq(idle.rgb, wait.rgb), "la pulsation garde la couleur du mode");
}

/* Un verdict est fugace et doit primer : c'est la seule chose qui distingue un
 * refus d'une panne. */
static void test_verdict_overrides_everything(void)
{
    led_view_t g = led_state_view(USB_MODE_PGP, true, LED_EVENT_GRANTED);
    TEST_ASSERT_EQ(g.pattern, LED_PATTERN_FLASH, "l'accord flashe malgre l'attente");
    TEST_ASSERT(rgb_eq(g.rgb, (led_rgb_t){LED_BRIGHT, LED_BRIGHT, LED_BRIGHT}),
                "l'accord est blanc");

    led_view_t r = led_state_view(USB_MODE_OTP, true, LED_EVENT_REFUSED);
    TEST_ASSERT_EQ(r.pattern, LED_PATTERN_FLASH, "le refus flashe aussi");
    TEST_ASSERT(rgb_eq(r.rgb, (led_rgb_t){LED_BRIGHT, 0, 0}), "le refus est rouge");
}

/* Accord et refus ne doivent jamais se ressembler : c'est tout l'interet. */
static void test_granted_and_refused_differ(void)
{
    led_view_t g = led_state_view(USB_MODE_PGP, true, LED_EVENT_GRANTED);
    led_view_t r = led_state_view(USB_MODE_PGP, true, LED_EVENT_REFUSED);
    TEST_ASSERT(!rgb_eq(g.rgb, r.rgb), "accord et refus sont distinguables");
}

/* Le repos est discret, le verdict se voit : c'est une cle, pas une lampe. */
static void test_verdict_is_brighter_than_rest(void)
{
    led_view_t idle = led_state_view(USB_MODE_OTP, false, LED_EVENT_NONE);
    led_view_t flash = led_state_view(USB_MODE_OTP, false, LED_EVENT_MODE);
    TEST_ASSERT(flash.rgb.g > idle.rgb.g, "le flash de bascule est plus lumineux");
}

/* otp -> pgp est la moitie des bascules reelles de la cle (l'autre etant
 * pgp -> otp) : sans ce test, seule la branche OTP de LED_EVENT_MODE etait
 * exercee, et une regression sur la couleur de bascule vers PGP serait passee
 * inapercue la moitie du temps. */
static void test_mode_flash_colour_for_pgp(void)
{
    led_view_t v = led_state_view(USB_MODE_PGP, false, LED_EVENT_MODE);
    TEST_ASSERT_EQ(v.pattern, LED_PATTERN_FLASH, "la bascule vers pgp flashe");
    TEST_ASSERT(rgb_eq(v.rgb, (led_rgb_t){0, 0, LED_BRIGHT}),
                "le flash de bascule vers pgp porte la couleur pgp en luminosite haute");
}

/* Comportement fige, pas change : une bascule vers un mode aberrant n'est
 * jamais emise en pratique (hmi.c n'appelle usb_mode_cycle_next() qu'apres
 * succes, et usb_mode_cycle_after() ne rend jamais que pgp ou otp), et la
 * spec ne demande pas d'indicateur "mode inconnu" dedie. Ce test fige donc le
 * flash noir resultant, pour qu'un futur changement de ce comportement soit
 * un choix delibere et non un accident silencieux. */
static void test_mode_flash_for_aberrant_mode_is_black_but_defined(void)
{
    led_view_t v = led_state_view((usb_mode_t)USB_MODE_COUNT, false, LED_EVENT_MODE);
    TEST_ASSERT_EQ(v.pattern, LED_PATTERN_FLASH, "meme aberrant, le motif reste flash");
    TEST_ASSERT(rgb_eq(v.rgb, (led_rgb_t){0, 0, 0}),
                "comportement fige : flash noir sur mode aberrant (delibere, cf commentaire)");
}

void test_led_state(void)
{
    TEST_SUITE("led_state");
    TEST_RUN(test_every_mode_has_a_view);
    TEST_RUN(test_none_is_dark);
    TEST_RUN(test_pgp_and_otp_differ);
    TEST_RUN(test_pulse_means_waiting_and_nothing_else);
    TEST_RUN(test_pulse_keeps_the_mode_colour);
    TEST_RUN(test_verdict_overrides_everything);
    TEST_RUN(test_granted_and_refused_differ);
    TEST_RUN(test_verdict_is_brighter_than_rest);
    TEST_RUN(test_mode_flash_colour_for_pgp);
    TEST_RUN(test_mode_flash_for_aberrant_mode_is_black_but_defined);
}
