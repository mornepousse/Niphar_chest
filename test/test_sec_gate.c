/* La béquille de confirmation ne doit exister que sur une carte SANS lien.
 * Ce test ne vérifie pas du code : il vérifie une décision de compilation,
 * et c'est justement ce qui peut se perdre en silence. */
#include "test_framework.h"

#include "sec_confirm.h"

/* Rejoue ce que fera sec_gate côté kit : un appui simulé n'accorde rien s'il
 * n'y a pas d'opération armée. La béquille ne doit pas être plus permissive
 * que la vraie touche. */
static void test_stub_is_not_more_permissive_than_a_key(void)
{
    sec_confirm_reset();
    sec_confirm_authorize();
    TEST_ASSERT_EQ(sec_confirm_poll(0, NULL), SEC_CONFIRM_IDLE,
                   "confirmation hors contexte sans effet");

    sec_confirm_arm(1, SEC_OP_UNKNOWN, 1000);
    sec_confirm_authorize();
    uint8_t slot = 0xFF;
    TEST_ASSERT_EQ(sec_confirm_poll(1100, &slot), SEC_CONFIRM_AUTHORIZED,
                   "confirmation après armement accordée");
    TEST_ASSERT_EQ(slot, 1, "emplacement conservé");
}

/* Une confirmation ne sert qu'une fois : rejouer l'appui ne doit pas
 * ré-autoriser une seconde opération. */
static void test_confirmation_is_single_use(void)
{
    sec_confirm_reset();
    sec_confirm_arm(2, SEC_OP_UNKNOWN, 1000);
    sec_confirm_authorize();
    TEST_ASSERT_EQ(sec_confirm_poll(1100, NULL), SEC_CONFIRM_AUTHORIZED, "premier usage");
    TEST_ASSERT_EQ(sec_confirm_poll(1200, NULL), SEC_CONFIRM_IDLE, "consommée");
}

void test_sec_gate(void)
{
    TEST_SUITE("sec_gate");
    TEST_RUN(test_stub_is_not_more_permissive_than_a_key);
    TEST_RUN(test_confirmation_is_single_use);
}
