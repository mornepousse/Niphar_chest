/* usb_mode est surtout du pilotage de matériel, mais son contrat de noms et de
 * bornes est pur — et c'est lui qui garantit qu'aucune fonction ne s'expose
 * sans avoir été demandée. */
#include "test_framework.h"

#include "usb/usb_mode.h"
#include "usb/usb_mode_state.h"

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

/* ------------------------------------------------------------------------ */
/* Ce que s_mode doit valoir quand une bascule échoue en cours de route.      */
/*                                                                            */
/* C'est le seul morceau PUR de usb_mode.c : une décision d'état, sans        */
/* matériel. Elle a déjà été fausse deux fois (revue finale, puis re-revue),  */
/* toujours de la même façon — annoncer USB_MODE_NONE alors que l'hôte voit   */
/* encore quelque chose. D'où sa sortie en en-tête et ces tests.              */
/* ------------------------------------------------------------------------ */

/*
 * Une désinstallation qui échoue n'a RIEN détaché : ni tud_disconnect(), ni
 * tusb_deinit(), ni le PHY (usb_device.c). L'hôte voit donc toujours le
 * périphérique du mode précédent. Annoncer NONE, c'est le mensonge que
 * usb_mode.h interdit.
 */
static void test_uninstall_failure_keeps_previous_mode(void)
{
    usb_mode_state_t st = usb_mode_state_on_failure(USB_MODE_PGP,
                                                    USB_MODE_FAIL_UNINSTALL);
    TEST_ASSERT_EQ(st.mode, USB_MODE_PGP,
                   "désinstallation en échec : l'hôte voit encore le mode PGP");
    TEST_ASSERT(!st.known, "et cet état n'est pas certain pour autant");
}

/*
 * La propriété dont dépend la sûreté du CCID, pas juste la cosmétique de
 * usb_mode_get() : c'est parce que s_mode reste à USB_MODE_PGP que la
 * tentative suivante repasse par mode_pgp_stop() — donc referme la porte des
 * callbacks AVANT le tusb_deinit() qui détruit la file de tud_task. Avec
 * USB_MODE_NONE, cette étape était sautée et le correctif du BLOQUANT 1 ne
 * tenait plus que par la rémanence de s_shutdown. Vrai pour tous les modes :
 * un mode futur avec son propre arrêt hérite de la garantie.
 */
static void test_uninstall_failure_never_forgets_the_mode(void)
{
    for (int m = 0; m < USB_MODE_COUNT; m++) {
        usb_mode_state_t st = usb_mode_state_on_failure((usb_mode_t)m,
                                                        USB_MODE_FAIL_UNINSTALL);
        TEST_ASSERT_EQ(st.mode, m,
                       "un échec de désinstallation ne change jamais le mode");
        TEST_ASSERT(!st.known,
                    "un échec de désinstallation ne rend jamais l'état certain");
    }
}

/*
 * L'autre versant : la désinstallation a réussi, c'est la suite (init du
 * disque, installation des descripteurs) qui a échoué. Là plus rien n'est
 * attaché, et NONE est la vérité — quel que soit le mode d'où l'on venait.
 */
static void test_failure_after_uninstall_reports_none(void)
{
    for (int m = 0; m < USB_MODE_COUNT; m++) {
        usb_mode_state_t st = usb_mode_state_on_failure((usb_mode_t)m,
                                                        USB_MODE_FAIL_AFTER_UNINSTALL);
        TEST_ASSERT_EQ(st.mode, USB_MODE_NONE,
                       "désinstallation réussie puis échec : plus rien n'est exposé");
        TEST_ASSERT(!st.known, "et l'état reste incertain");
    }
}

/*
 * Aucun chemin d'échec ne rend un état certain. C'est ce qui garantit qu'une
 * nouvelle demande du MÊME mode n'est pas court-circuitée par le « déjà
 * courant » de usb_mode_set() : sans ça, la reprise serait impossible et le
 * coffre resterait coincé dans un mode qu'il croit servir.
 */
static void test_no_failure_path_claims_certainty(void)
{
    const usb_mode_fail_t wheres[] = { USB_MODE_FAIL_UNINSTALL,
                                       USB_MODE_FAIL_AFTER_UNINSTALL };
    for (unsigned w = 0; w < sizeof(wheres) / sizeof(wheres[0]); w++) {
        for (int m = 0; m < USB_MODE_COUNT; m++) {
            usb_mode_state_t st = usb_mode_state_on_failure((usb_mode_t)m, wheres[w]);
            TEST_ASSERT(!st.known,
                        "un échec ne rend jamais l'état certain, quel qu'il soit");
        }
    }
}

/* Le mode rendu reste toujours une valeur valide de l'énumération : il part
 * dans usb_mode_name() et dans la comparaison « déjà courant ». */
static void test_reported_mode_stays_in_range(void)
{
    const usb_mode_fail_t wheres[] = { USB_MODE_FAIL_UNINSTALL,
                                       USB_MODE_FAIL_AFTER_UNINSTALL };
    for (unsigned w = 0; w < sizeof(wheres) / sizeof(wheres[0]); w++) {
        for (int m = 0; m < USB_MODE_COUNT; m++) {
            usb_mode_state_t st = usb_mode_state_on_failure((usb_mode_t)m, wheres[w]);
            TEST_ASSERT(st.mode >= USB_MODE_NONE && st.mode < USB_MODE_COUNT,
                        "le mode rendu reste dans les bornes de l'énumération");
        }
    }
}

void test_usb_mode(void)
{
    TEST_SUITE("usb_mode");
    TEST_RUN(test_name_never_null);
    TEST_RUN(test_none_is_zero);
    TEST_RUN(test_names_are_distinct);
    TEST_RUN(test_uninstall_failure_keeps_previous_mode);
    TEST_RUN(test_uninstall_failure_never_forgets_the_mode);
    TEST_RUN(test_failure_after_uninstall_reports_none);
    TEST_RUN(test_no_failure_path_claims_certainty);
    TEST_RUN(test_reported_mode_stays_in_range);
}
