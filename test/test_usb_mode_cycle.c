/* Le cycle de la carte-clé : trois crans (pgp, otp, fido), et « rien
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

/* Les trois crans, dans l'ordre, et refermes sur eux-memes : chaque
 * transition verifiee explicitement, pas seulement « ca boucle », pour que
 * mordre une mutation qui echangerait deux destinations (ex. otp -> pgp,
 * fido -> otp) ne reste pas invisible. */
static void test_pgp_otp_fido_cycle(void)
{
    TEST_ASSERT_EQ(usb_mode_cycle_after(USB_MODE_PGP), USB_MODE_OTP, "pgp -> otp");
    TEST_ASSERT_EQ(usb_mode_cycle_after(USB_MODE_OTP), USB_MODE_FIDO, "otp -> fido");
    TEST_ASSERT_EQ(usb_mode_cycle_after(USB_MODE_FIDO), USB_MODE_PGP, "fido -> pgp");
}

/* Les trois destinations comparees deux a deux — pas chacune a elle-meme :
 * le piege recurrent de ce projet est un test qui croit couvrir plusieurs
 * cas alors qu'il ne compare qu'une valeur a une constante attendue,
 * laissant passer deux enumerateurs qui se vaudraient. */
static void test_the_three_steps_are_pairwise_distinct(void)
{
    const usb_mode_t after_pgp  = usb_mode_cycle_after(USB_MODE_PGP);
    const usb_mode_t after_otp  = usb_mode_cycle_after(USB_MODE_OTP);
    const usb_mode_t after_fido = usb_mode_cycle_after(USB_MODE_FIDO);

    TEST_ASSERT(after_pgp != after_otp, "pgp et otp ne menent pas au meme cran");
    TEST_ASSERT(after_pgp != after_fido, "pgp et fido ne menent pas au meme cran");
    TEST_ASSERT(after_otp != after_fido, "otp et fido ne menent pas au meme cran");
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
    TEST_RUN(test_pgp_otp_fido_cycle);
    TEST_RUN(test_the_three_steps_are_pairwise_distinct);
    TEST_RUN(test_never_returns_to_none);
    TEST_RUN(test_aberrant_input_lands_on_pgp);
    TEST_RUN(test_storage_is_not_in_the_cycle);
}
