/* Le cycle de la carte-clé : deux crans, et « rien d'exposé » n'y revient
 * jamais. C'est pur, donc c'est ici que ça se prouve. */
#include "test_framework.h"

#include "usb/usb_mode.h"
#include "usb/usb_mode_cycle.h"

/* Au branchement la clé est muette ; le premier appui doit l'armer. */
static void test_none_arms_pgp(void)
{
    TEST_ASSERT_EQ(usb_mode_cycle_after(USB_MODE_NONE), USB_MODE_PGP,
                   "depuis l'etat muet, le premier appui arme PGP");
}

static void test_pgp_otp_alternate(void)
{
    TEST_ASSERT_EQ(usb_mode_cycle_after(USB_MODE_PGP), USB_MODE_OTP, "pgp -> otp");
    TEST_ASSERT_EQ(usb_mode_cycle_after(USB_MODE_OTP), USB_MODE_PGP, "otp -> pgp");
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
}

void test_usb_mode_cycle(void)
{
    TEST_SUITE("usb_mode_cycle");
    TEST_RUN(test_none_arms_pgp);
    TEST_RUN(test_pgp_otp_alternate);
    TEST_RUN(test_never_returns_to_none);
    TEST_RUN(test_aberrant_input_lands_on_pgp);
    TEST_RUN(test_storage_is_not_in_the_cycle);
}
