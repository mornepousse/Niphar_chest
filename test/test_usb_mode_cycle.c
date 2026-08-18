/* Le cycle de la carte-clé : quatre crans (pgp, otp, fido, oath), et « rien
 * d'exposé » n'y revient jamais. C'est pur, donc c'est ici que ça se
 * prouve. */
#include "test_framework.h"

#include "usb/usb_mode.h"
#include "usb/usb_mode_cycle.h"

/* Au branchement la clé est muette ; le premier appui doit l'armer. */
static void test_none_arms_pgp(void)
{
    TEST_ASSERT_EQ(usb_mode_cycle_after(USB_MODE_NONE), USB_MODE_PGP,
                   "depuis l'etat muet, le premier appui arme PGP");
}

/* Les quatre crans, dans l'ordre, et refermes sur eux-memes : chaque
 * transition verifiee explicitement, pas seulement « ca boucle », pour que
 * mordre une mutation qui echangerait deux destinations (ex. otp -> pgp,
 * fido -> otp) ne reste pas invisible. */
static void test_pgp_otp_fido_oath_cycle(void)
{
    TEST_ASSERT_EQ(usb_mode_cycle_after(USB_MODE_PGP), USB_MODE_OTP, "pgp -> otp");
    TEST_ASSERT_EQ(usb_mode_cycle_after(USB_MODE_OTP), USB_MODE_FIDO, "otp -> fido");
    TEST_ASSERT_EQ(usb_mode_cycle_after(USB_MODE_FIDO), USB_MODE_OATH, "fido -> oath");
    TEST_ASSERT_EQ(usb_mode_cycle_after(USB_MODE_OATH), USB_MODE_PGP, "oath -> pgp");
}

/* Les quatre destinations comparees deux a deux — pas chacune a elle-meme :
 * le piege recurrent de ce projet est un test qui croit couvrir plusieurs
 * cas alors qu'il ne compare qu'une valeur a une constante attendue,
 * laissant passer deux enumerateurs qui se vaudraient. */
static void test_the_four_steps_are_pairwise_distinct(void)
{
    const usb_mode_t suivants[] = {
        usb_mode_cycle_after(USB_MODE_PGP),
        usb_mode_cycle_after(USB_MODE_OTP),
        usb_mode_cycle_after(USB_MODE_FIDO),
        usb_mode_cycle_after(USB_MODE_OATH),
    };
    const unsigned n = sizeof(suivants) / sizeof(suivants[0]);
    for (unsigned i = 0; i < n; i++) {
        for (unsigned j = i + 1; j < n; j++) {
            TEST_ASSERT(suivants[i] != suivants[j],
                        "deux crans ne menent pas au meme mode");
        }
    }
}

/*
 * Le cycle atteint REELLEMENT ses quatre crans depuis le point de depart.
 * Sans ce test, un cycle amoindri a trois (oath oublie dans le switch, donc
 * capte par le `default:` qui rend PGP) resterait vert partout ailleurs : les
 * transitions ci-dessus l'attraperaient, mais rien ne dirait que le PARCOURS
 * — ce que l'ecran compte pour dessiner ses points — visite bien quatre
 * modes distincts avant de se refermer.
 */
static void test_the_walk_visits_four_distinct_modes(void)
{
    usb_mode_t vus[USB_MODE_COUNT];
    unsigned n = 0;
    usb_mode_t m = usb_mode_cycle_after(USB_MODE_NONE);
    const usb_mode_t depart = m;

    do {
        TEST_ASSERT(n < USB_MODE_COUNT, "le parcours ne depasse pas le domaine");
        if (n >= USB_MODE_COUNT) break;
        vus[n++] = m;
        m = usb_mode_cycle_after(m);
    } while (m != depart);

    TEST_ASSERT_EQ(n, 4u, "le parcours compte quatre crans distincts");
    for (unsigned i = 0; i < n; i++) {
        for (unsigned j = i + 1; j < n; j++) {
            TEST_ASSERT(vus[i] != vus[j], "aucun mode n'est visite deux fois");
        }
    }
}

/* Le point qui compte : une clé ne doit pas retomber muette toute seule.
 * « none » ne s'atteint qu'en debranchant. */
static void test_never_returns_to_none(void)
{
    usb_mode_t m = USB_MODE_NONE;
    for (int i = 0; i < 32; i++) {
        m = usb_mode_cycle_after(m);
        TEST_ASSERT(m != USB_MODE_NONE, "le cycle ne rend jamais l'etat muet");
    }
}

/* Un mode aberrant (memoire corrompue, futur mode non gere) doit atterrir sur
 * une valeur sure et connue, pas propager l'aberration. */
static void test_aberrant_input_lands_on_pgp(void)
{
    TEST_ASSERT_EQ(usb_mode_cycle_after((usb_mode_t)USB_MODE_COUNT), USB_MODE_PGP,
                   "une valeur hors bornes arme PGP");
    TEST_ASSERT_EQ(usb_mode_cycle_after((usb_mode_t)255), USB_MODE_PGP,
                   "une valeur aberrante arme PGP");
}

/* storage n'est pas dans le cycle de cette carte : elle n'a pas de microSD. */
static void test_storage_is_not_in_the_cycle(void)
{
    usb_mode_t m = USB_MODE_NONE;
    for (int i = 0; i < 32; i++) {
        m = usb_mode_cycle_after(m);
        TEST_ASSERT(m != USB_MODE_STORAGE, "storage n'entre jamais dans le cycle");
    }

    /* En entrée directe aussi : STORAGE est une valeur valide du domaine
     * (contrairement à USB_MODE_COUNT ou 255), et sur la carte-clé,
     * BOARD_CONSOLE_ACTIONS laisse `usb mode storage` disponible en console —
     * s_mode peut donc réellement valoir STORAGE au moment où le bouton
     * appelle le cycle. Elle doit rejoindre pgp, pas l'état muet. */
    TEST_ASSERT_EQ(usb_mode_cycle_after(USB_MODE_STORAGE), USB_MODE_PGP,
                   "depuis storage, le cycle rejoint pgp et non l'etat muet");
}

void test_usb_mode_cycle(void)
{
    TEST_SUITE("usb_mode_cycle");
    TEST_RUN(test_none_arms_pgp);
    TEST_RUN(test_pgp_otp_fido_oath_cycle);
    TEST_RUN(test_the_four_steps_are_pairwise_distinct);
    TEST_RUN(test_the_walk_visits_four_distinct_modes);
    TEST_RUN(test_never_returns_to_none);
    TEST_RUN(test_aberrant_input_lands_on_pgp);
    TEST_RUN(test_storage_is_not_in_the_cycle);
}
