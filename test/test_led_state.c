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

/* L'alternance ne doit signifier qu'une chose : « j'attends ton doigt ». */
static void test_alternate_means_waiting_and_nothing_else(void)
{
    TEST_ASSERT_EQ(led_state_view(USB_MODE_PGP, true, LED_EVENT_NONE).pattern,
                   LED_PATTERN_ALTERNATE, "attente en pgp -> alternance");
    TEST_ASSERT_EQ(led_state_view(USB_MODE_OTP, true, LED_EVENT_NONE).pattern,
                   LED_PATTERN_ALTERNATE, "attente en otp -> alternance");
    TEST_ASSERT_EQ(led_state_view(USB_MODE_PGP, false, LED_EVENT_NONE).pattern,
                   LED_PATTERN_STEADY, "hors attente, la LED est fixe");
}

/* Constat sur materiel (2026-08-17) : une pulsation 0->20/255 est trop
 * discrete, elle se rate si on ne fixe pas la LED. L'attente garde la TEINTE
 * du mode — le canal qui porte la couleur reste le meme — mais monte a pleine
 * luminosite, et alterne desormais avec le rouge plutot que de moduler
 * l'intensite. Ce test remplace l'ancien test_pulse_keeps_the_mode_colour,
 * qui affirmait a tort que repos et attente etaient la MEME couleur : ce
 * n'est plus vrai depuis ce changement, par decision explicite. */
static void test_alternate_keeps_the_mode_hue_but_raises_brightness(void)
{
    led_view_t idle = led_state_view(USB_MODE_PGP, false, LED_EVENT_NONE);
    led_view_t wait = led_state_view(USB_MODE_PGP, true,  LED_EVENT_NONE);

    TEST_ASSERT_EQ(wait.pattern, LED_PATTERN_ALTERNATE, "l'attente alterne");
    TEST_ASSERT((idle.rgb.r > 0) == (wait.rgb.r > 0), "meme canal rouge actif qu'au repos");
    TEST_ASSERT((idle.rgb.g > 0) == (wait.rgb.g > 0), "meme canal vert actif qu'au repos");
    TEST_ASSERT((idle.rgb.b > 0) == (wait.rgb.b > 0), "meme canal bleu actif qu'au repos");
    TEST_ASSERT(!rgb_eq(idle.rgb, wait.rgb),
                "mais l'attente n'est plus a la meme luminosite que le repos");
    TEST_ASSERT(rgb_eq(wait.rgb, (led_rgb_t){0, 0, LED_BRIGHT}),
                "l'attente en pgp est bleu a pleine luminosite");
}

/* L'alternance n'existe que si les deux couleurs different : sinon rien ne
 * clignote, et c'est precisement le defaut qui viderait le changement de son
 * sens (une pulsation ratee remplacee par une alternance tout aussi ratee). */
static void test_alternate_colours_differ_and_are_both_visible(void)
{
    led_view_t pgp = led_state_view(USB_MODE_PGP, true, LED_EVENT_NONE);
    led_view_t otp = led_state_view(USB_MODE_OTP, true, LED_EVENT_NONE);

    TEST_ASSERT(!rgb_eq(pgp.rgb, pgp.rgb_alt), "pgp : les deux couleurs de l'alternance different");
    TEST_ASSERT(!rgb_eq(pgp.rgb, (led_rgb_t){0, 0, 0}), "pgp : rgb est visible");
    TEST_ASSERT(!rgb_eq(pgp.rgb_alt, (led_rgb_t){0, 0, 0}), "pgp : rgb_alt est visible");

    TEST_ASSERT(!rgb_eq(otp.rgb, otp.rgb_alt), "otp : les deux couleurs de l'alternance different");
    TEST_ASSERT(!rgb_eq(otp.rgb, (led_rgb_t){0, 0, 0}), "otp : rgb est visible");
    TEST_ASSERT(!rgb_eq(otp.rgb_alt, (led_rgb_t){0, 0, 0}), "otp : rgb_alt est visible");
}

/* rgb_alt en attente est le rouge, a pleine luminosite : c'est le second
 * terme de l'alternance demandee, quel que soit le mode. */
static void test_alternate_alt_colour_is_bright_red(void)
{
    led_view_t pgp = led_state_view(USB_MODE_PGP, true, LED_EVENT_NONE);
    led_view_t otp = led_state_view(USB_MODE_OTP, true, LED_EVENT_NONE);

    TEST_ASSERT(rgb_eq(pgp.rgb_alt, (led_rgb_t){LED_BRIGHT, 0, 0}),
                "attente pgp : rgb_alt est le rouge plein");
    TEST_ASSERT(rgb_eq(otp.rgb_alt, (led_rgb_t){LED_BRIGHT, 0, 0}),
                "attente otp : rgb_alt est le rouge plein");
}

/* Hors attente, rgb_alt doit valoir rgb : un consommateur qui ignorerait le
 * motif ne doit jamais produire un clignotement fantome. */
static void test_non_pending_rgb_alt_equals_rgb(void)
{
    led_view_t off    = led_state_view(USB_MODE_NONE, false, LED_EVENT_NONE);
    led_view_t steady = led_state_view(USB_MODE_PGP,  false, LED_EVENT_NONE);
    led_view_t granted = led_state_view(USB_MODE_PGP, true,  LED_EVENT_GRANTED);
    led_view_t refused = led_state_view(USB_MODE_OTP, true,  LED_EVENT_REFUSED);
    led_view_t mode_ev = led_state_view(USB_MODE_OTP, false, LED_EVENT_MODE);

    TEST_ASSERT(rgb_eq(off.rgb, off.rgb_alt), "off : rgb_alt == rgb");
    TEST_ASSERT(rgb_eq(steady.rgb, steady.rgb_alt), "steady : rgb_alt == rgb");
    TEST_ASSERT(rgb_eq(granted.rgb, granted.rgb_alt), "flash accord : rgb_alt == rgb");
    TEST_ASSERT(rgb_eq(refused.rgb, refused.rgb_alt), "flash refus : rgb_alt == rgb");
    TEST_ASSERT(rgb_eq(mode_ev.rgb, mode_ev.rgb_alt), "flash bascule : rgb_alt == rgb");
}

/* L'attente en otp doit alterner vert/rouge, et se distinguer de l'attente en
 * pgp (qui alterne bleu/rouge) : deux modes qui attendraient la meme paire de
 * couleurs seraient aussi confondus qu'au repos. */
static void test_otp_alternate_differs_from_pgp_alternate(void)
{
    led_view_t otp = led_state_view(USB_MODE_OTP, true, LED_EVENT_NONE);
    led_view_t pgp = led_state_view(USB_MODE_PGP, true, LED_EVENT_NONE);

    TEST_ASSERT(rgb_eq(otp.rgb, (led_rgb_t){0, LED_BRIGHT, 0}), "attente otp : vert plein");
    TEST_ASSERT(rgb_eq(otp.rgb_alt, (led_rgb_t){LED_BRIGHT, 0, 0}), "attente otp : rouge plein");
    TEST_ASSERT(!rgb_eq(otp.rgb, pgp.rgb), "otp et pgp n'attendent pas avec la meme couleur de mode");
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
 * spec ne demande pas d'indicateur "mode inconnu" dedie. Les deux assertions
 * ci-dessous ne gelent pas la meme chose :
 *
 * - la premiere gele qu'un evenement flashe TOUJOURS, quel que soit le mode
 *   - y compris un mode inconnu. C'est le garde-fou mecanique du refus
 *   explicite d'ajouter un indicateur "mode inconnu" (qui aurait justement
 *   consiste a NE PAS flasher normalement dans ce cas) : si quelqu'un
 *   implemente un jour ce comportement, cette assertion rougit, et il faudra
 *   que ce soit une decision deliberee, pas un accident silencieux ;
 * - la seconde gele que la couleur de repli d'un mode inconnu est le noir,
 *   c'est-a-dire le comportement de led_mode_colour() sur son cas `default`.
 *
 * Une mutation de couleur (STORAGE, PGP, OTP) ne peut faire rougir que la
 * seconde : la premiere ne depend que de la presence d'un evenement, calculee
 * avant tout appel a led_mode_colour(). Seule une mutation qui touche la
 * condition posant LED_PATTERN_FLASH elle-meme peut l'eprouver. */
static void test_mode_flash_for_aberrant_mode_is_black_but_defined(void)
{
    led_view_t v = led_state_view((usb_mode_t)USB_MODE_COUNT, false, LED_EVENT_MODE);
    TEST_ASSERT_EQ(v.pattern, LED_PATTERN_FLASH,
                   "un evenement flashe toujours, meme pour un mode inconnu (refus delibere d'un indicateur dedie)");
    TEST_ASSERT(rgb_eq(v.rgb, (led_rgb_t){0, 0, 0}),
                "et la couleur de repli d'un mode inconnu est le noir");
}

/*
 * led_wait_phase() — la phase de l'alternance est RELATIVE a l'instant
 * d'armement, jamais absolue. Avant cette fonction, hmi.c calculait
 * `t % HMI_ALTERNATE_MS` sur l'horloge murale : une attente qui s'armait
 * dans la seconde moitie d'une periode demarrait donc sur rgb_alt (le
 * rouge) — exactement le cas que la spec voulait eviter, puisque le rouge
 * signifie deja "refuse" ailleurs (le flash de 120 ms). La garantie que ces
 * tests protegent : la PREMIERE phase vue par l'utilisateur, quel que soit
 * l'instant d'armement, est toujours celle du mode (rgb, primary).
 */

/* A l'instant meme de l'armement, sans aucun ecart : toujours primary. */
static void test_wait_phase_starts_on_primary_at_arming(void)
{
    TEST_ASSERT_EQ(led_wait_phase(0, 0), LED_WAIT_PHASE_PRIMARY,
                   "phase a l'armement (t=0) : primary");
    TEST_ASSERT_EQ(led_wait_phase(12345, 12345), LED_WAIT_PHASE_PRIMARY,
                   "phase a l'armement, quel que soit l'instant absolu : primary");
}

/* Toute la premiere moitie de periode doit rester primary — pas un instant
 * isole, toute la fenetre. */
static void test_wait_phase_stays_primary_through_first_half(void)
{
    const uint32_t armed = 7000;
    for (uint32_t dt = 0; dt < LED_ALTERNATE_MS / 2; dt += 37) {
        TEST_ASSERT_EQ(led_wait_phase(armed, armed + dt), LED_WAIT_PHASE_PRIMARY,
                       "premiere demi-periode : toujours primary");
    }
}

/* Bascule pile a la moitie, et reste alt jusqu'a la fin de la periode. */
static void test_wait_phase_switches_to_alt_at_half_period(void)
{
    const uint32_t armed = 7000;
    TEST_ASSERT_EQ(led_wait_phase(armed, armed + LED_ALTERNATE_MS / 2), LED_WAIT_PHASE_ALT,
                   "pile a la moitie de periode : bascule vers alt");
    for (uint32_t dt = LED_ALTERNATE_MS / 2; dt < LED_ALTERNATE_MS; dt += 41) {
        TEST_ASSERT_EQ(led_wait_phase(armed, armed + dt), LED_WAIT_PHASE_ALT,
                       "seconde demi-periode : toujours alt");
    }
}

/* La periode se repete : la phase a armed+LED_ALTERNATE_MS doit redevenir
 * primary, exactement comme a l'armement. */
static void test_wait_phase_repeats_every_period(void)
{
    const uint32_t armed = 500;
    TEST_ASSERT_EQ(led_wait_phase(armed, armed + LED_ALTERNATE_MS), LED_WAIT_PHASE_PRIMARY,
                   "une periode plus tard : de nouveau primary");
    TEST_ASSERT_EQ(led_wait_phase(armed, armed + LED_ALTERNATE_MS + LED_ALTERNATE_MS / 2),
                   LED_WAIT_PHASE_ALT, "une periode et demie plus tard : alt");
}

/* Le compteur de millisecondes d'ESP-IDF repasse par zero apres ~49 jours
 * (uint32_t). Une cle peut rester branchee, et une attente peut s'armer,
 * plus longtemps que ca. Meme construction que
 * test_survives_millisecond_wraparound dans test_button_debounce.c. */
static void test_wait_phase_survives_millisecond_wraparound(void)
{
    const uint32_t armed = 0xFFFFFFF0u;   /* arme 16 ms avant le repassage a zero */
    TEST_ASSERT_EQ(led_wait_phase(armed, armed), LED_WAIT_PHASE_PRIMARY,
                   "arme juste avant le repassage a zero : primary");
    /* +100 ms depuis l'armement, en traversant le repassage a zero : toujours
     * dans la premiere demi-periode (100 < LED_ALTERNATE_MS / 2). */
    TEST_ASSERT_EQ(led_wait_phase(armed, armed + 100), LED_WAIT_PHASE_PRIMARY,
                   "premiere demi-periode a cheval sur le repassage a zero : primary");
    /* +600 ms : dans la seconde demi-periode (LED_ALTERNATE_MS/2 = 500). */
    TEST_ASSERT_EQ(led_wait_phase(armed, armed + 600), LED_WAIT_PHASE_ALT,
                   "seconde demi-periode a cheval sur le repassage a zero : alt");
}

void test_led_state(void)
{
    TEST_SUITE("led_state");
    TEST_RUN(test_every_mode_has_a_view);
    TEST_RUN(test_none_is_dark);
    TEST_RUN(test_pgp_and_otp_differ);
    TEST_RUN(test_alternate_means_waiting_and_nothing_else);
    TEST_RUN(test_alternate_keeps_the_mode_hue_but_raises_brightness);
    TEST_RUN(test_alternate_colours_differ_and_are_both_visible);
    TEST_RUN(test_alternate_alt_colour_is_bright_red);
    TEST_RUN(test_non_pending_rgb_alt_equals_rgb);
    TEST_RUN(test_otp_alternate_differs_from_pgp_alternate);
    TEST_RUN(test_verdict_overrides_everything);
    TEST_RUN(test_granted_and_refused_differ);
    TEST_RUN(test_verdict_is_brighter_than_rest);
    TEST_RUN(test_mode_flash_colour_for_pgp);
    TEST_RUN(test_mode_flash_for_aberrant_mode_is_black_but_defined);
    TEST_RUN(test_wait_phase_starts_on_primary_at_arming);
    TEST_RUN(test_wait_phase_stays_primary_through_first_half);
    TEST_RUN(test_wait_phase_switches_to_alt_at_half_period);
    TEST_RUN(test_wait_phase_repeats_every_period);
    TEST_RUN(test_wait_phase_survives_millisecond_wraparound);
}
