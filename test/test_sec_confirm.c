#include "test_framework.h"
#include "sec_confirm.h"

static void test_arm_authorize_consume(void)
{
    sec_confirm_reset();
    sec_confirm_arm(2, 1000);
    TEST_ASSERT_EQ(sec_confirm_poll(1000, NULL), SEC_CONFIRM_PENDING, "armed -> PENDING");
    sec_confirm_authorize();
    uint8_t slot = 0xFF;
    TEST_ASSERT_EQ(sec_confirm_poll(1100, &slot), SEC_CONFIRM_AUTHORIZED, "authorized");
    TEST_ASSERT_EQ(slot, 2, "slot preserved");
    TEST_ASSERT_EQ(sec_confirm_poll(1200, NULL), SEC_CONFIRM_IDLE, "consumed -> IDLE");
}

static void test_timeout(void)
{
    sec_confirm_reset();
    sec_confirm_arm(0, 1000);
    TEST_ASSERT_EQ(sec_confirm_poll(1000 + 14999, NULL), SEC_CONFIRM_PENDING, "before timeout");
    TEST_ASSERT_EQ(sec_confirm_poll(1000 + 15000, NULL), SEC_CONFIRM_TIMEDOUT, "at timeout");
    TEST_ASSERT_EQ(sec_confirm_poll(1000 + 16000, NULL), SEC_CONFIRM_IDLE, "after timeout -> IDLE");
}

static void test_authorize_without_arm(void)
{
    sec_confirm_reset();
    sec_confirm_authorize();
    TEST_ASSERT_EQ(sec_confirm_poll(0, NULL), SEC_CONFIRM_IDLE, "authorize w/o arm = no-op");
}

static void test_rearm_overwrites_slot(void)
{
    sec_confirm_reset();
    sec_confirm_arm(1, 1000);
    sec_confirm_arm(3, 1050);
    sec_confirm_authorize();
    uint8_t slot = 0xFF;
    TEST_ASSERT_EQ(sec_confirm_poll(1060, &slot), SEC_CONFIRM_AUTHORIZED, "re-arm then authorize -> AUTHORIZED");
    TEST_ASSERT_EQ(slot, 3, "re-arm overwrites slot");
}

static void test_arm_while_authorized(void)
{
    sec_confirm_reset();
    sec_confirm_arm(1, 1000);
    sec_confirm_authorize();          /* AUTHORIZED, not yet polled */
    sec_confirm_arm(2, 1100);         /* re-arm discards the grant */
    uint8_t slot = 0xFF;
    TEST_ASSERT_EQ(sec_confirm_poll(1100, &slot), SEC_CONFIRM_PENDING,
                   "arm after authorize discards grant -> PENDING");
}

/* peek() existe pour l'affichage. S'il consommait quoi que ce soit, la tache
 * qui allume la LED volerait la permission a celle qui attend de signer — et
 * l'echec serait muet, donc inexplicable. */
static void test_peek_does_not_consume_the_grant(void)
{
    sec_confirm_reset();
    sec_confirm_arm(7, 1000);
    sec_confirm_authorize();

    TEST_ASSERT_EQ(sec_confirm_peek(1100), SEC_CONFIRM_AUTHORIZED,
                   "peek voit l'autorisation");
    TEST_ASSERT_EQ(sec_confirm_peek(1100), SEC_CONFIRM_AUTHORIZED,
                   "et la voit encore : il ne consomme rien");

    uint8_t slot = 0;
    TEST_ASSERT_EQ(sec_confirm_poll(1100, &slot), SEC_CONFIRM_AUTHORIZED,
                   "poll recoit l'autorisation intacte apres deux peek");
    TEST_ASSERT_EQ(slot, 7, "et le bon slot avec");
}

/* Meme exigence sur l'expiration : la voir ne doit pas la declencher. */
static void test_peek_reports_timeout_without_clearing_it(void)
{
    sec_confirm_reset();
    sec_confirm_arm(3, 1000);

    TEST_ASSERT_EQ(sec_confirm_peek(1000 + SEC_CONFIRM_TIMEOUT_MS),
                   SEC_CONFIRM_TIMEDOUT, "peek voit l'expiration");
    TEST_ASSERT_EQ(sec_confirm_peek(1000 + SEC_CONFIRM_TIMEOUT_MS),
                   SEC_CONFIRM_TIMEDOUT, "et la voit toujours");
    TEST_ASSERT_EQ(sec_confirm_poll(1000 + SEC_CONFIRM_TIMEOUT_MS, NULL),
                   SEC_CONFIRM_TIMEDOUT,
                   "poll rend l'expiration, que peek n'avait pas consommee");
}

static void test_peek_sees_pending_before_timeout(void)
{
    sec_confirm_reset();
    sec_confirm_arm(1, 1000);
    TEST_ASSERT_EQ(sec_confirm_peek(1000 + SEC_CONFIRM_TIMEOUT_MS - 1),
                   SEC_CONFIRM_PENDING, "avant l'echeance, l'operation est en attente");
}

static void test_peek_on_idle_is_idle(void)
{
    sec_confirm_reset();
    TEST_ASSERT_EQ(sec_confirm_peek(50000), SEC_CONFIRM_IDLE,
                   "rien d'arme : rien a montrer");
}

void test_sec_confirm(void)
{
    TEST_SUITE("sec_confirm state machine");
    TEST_RUN(test_arm_authorize_consume);
    TEST_RUN(test_timeout);
    TEST_RUN(test_authorize_without_arm);
    TEST_RUN(test_rearm_overwrites_slot);
    TEST_RUN(test_arm_while_authorized);
    TEST_RUN(test_peek_does_not_consume_the_grant);
    TEST_RUN(test_peek_reports_timeout_without_clearing_it);
    TEST_RUN(test_peek_sees_pending_before_timeout);
    TEST_RUN(test_peek_on_idle_is_idle);
}
