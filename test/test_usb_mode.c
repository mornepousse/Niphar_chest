/* usb_mode est surtout du pilotage de matériel, mais son contrat de noms et de
 * bornes est pur — et c'est lui qui garantit qu'aucune fonction ne s'expose
 * sans avoir été demandée. */
#include "test_framework.h"

#include "usb/usb_mode.h"

static void test_name_never_null(void)
{
    for (int m = 0; m < USB_MODE_COUNT; m++) {
        TEST_ASSERT(usb_mode_name((usb_mode_t)m) != NULL, "chaque mode a un nom");
    }
    TEST_ASSERT(usb_mode_name((usb_mode_t)USB_MODE_COUNT) != NULL,
                "un mode hors bornes rend quand même un nom");
    TEST_ASSERT(usb_mode_name((usb_mode_t)255) != NULL,
                "une valeur aberrante rend quand même un nom");
}

/* Le mode 0 doit être NONE : c'est la valeur d'une variable statique non
 * initialisée, donc l'état par défaut doit être le plus sûr. */
static void test_none_is_zero(void)
{
    TEST_ASSERT_EQ(USB_MODE_NONE, 0, "NONE vaut zéro, l'état par défaut sûr");
}

static void test_names_are_distinct(void)
{
    for (int a = 0; a < USB_MODE_COUNT; a++) {
        for (int b = a + 1; b < USB_MODE_COUNT; b++) {
            TEST_ASSERT(strcmp(usb_mode_name((usb_mode_t)a),
                               usb_mode_name((usb_mode_t)b)) != 0,
                        "deux modes ne portent pas le même nom");
        }
    }
}

void test_usb_mode(void)
{
    TEST_SUITE("usb_mode");
    TEST_RUN(test_name_never_null);
    TEST_RUN(test_none_is_zero);
    TEST_RUN(test_names_are_distinct);
}
