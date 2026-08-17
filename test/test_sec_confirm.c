#include "test_framework.h"
#include "sec_confirm.h"

static void test_arm_authorize_consume(void)
{
    sec_confirm_reset();
    sec_confirm_arm(2, SEC_OP_UNKNOWN, 1000);
    TEST_ASSERT_EQ(sec_confirm_poll(1000, NULL), SEC_CONFIRM_PENDING, "armed -> PENDING");
    sec_confirm_authorize(1050);
    uint8_t slot = 0xFF;
    TEST_ASSERT_EQ(sec_confirm_poll(1100, &slot), SEC_CONFIRM_AUTHORIZED, "authorized");
    TEST_ASSERT_EQ(slot, 2, "slot preserved");
    TEST_ASSERT_EQ(sec_confirm_poll(1200, NULL), SEC_CONFIRM_IDLE, "consumed -> IDLE");
}

static void test_timeout(void)
{
    sec_confirm_reset();
    sec_confirm_arm(0, SEC_OP_UNKNOWN, 1000);
    TEST_ASSERT_EQ(sec_confirm_poll(1000 + 14999, NULL), SEC_CONFIRM_PENDING, "before timeout");
    TEST_ASSERT_EQ(sec_confirm_poll(1000 + 15000, NULL), SEC_CONFIRM_TIMEDOUT, "at timeout");
    TEST_ASSERT_EQ(sec_confirm_poll(1000 + 16000, NULL), SEC_CONFIRM_IDLE, "after timeout -> IDLE");
}

static void test_authorize_without_arm(void)
{
    sec_confirm_reset();
    sec_confirm_authorize(1000);
    TEST_ASSERT_EQ(sec_confirm_poll(0, NULL), SEC_CONFIRM_IDLE, "authorize w/o arm = no-op");
}

static void test_rearm_overwrites_slot(void)
{
    sec_confirm_reset();
    sec_confirm_arm(1, SEC_OP_UNKNOWN, 1000);
    sec_confirm_arm(3, SEC_OP_UNKNOWN, 1050);
    sec_confirm_authorize(1055);
    uint8_t slot = 0xFF;
    TEST_ASSERT_EQ(sec_confirm_poll(1060, &slot), SEC_CONFIRM_AUTHORIZED, "re-arm then authorize -> AUTHORIZED");
    TEST_ASSERT_EQ(slot, 3, "re-arm overwrites slot");
}

static void test_arm_while_authorized(void)
{
    sec_confirm_reset();
    sec_confirm_arm(1, SEC_OP_UNKNOWN, 1000);
    sec_confirm_authorize(1050);      /* AUTHORIZED, not yet polled */
    sec_confirm_arm(2, SEC_OP_UNKNOWN, 1100);         /* re-arm discards the grant */
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
    sec_confirm_arm(7, SEC_OP_UNKNOWN, 1000);
    sec_confirm_authorize(1050);

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
    sec_confirm_arm(3, SEC_OP_UNKNOWN, 1000);

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
    sec_confirm_arm(1, SEC_OP_UNKNOWN, 1000);
    TEST_ASSERT_EQ(sec_confirm_peek(1000 + SEC_CONFIRM_TIMEOUT_MS - 1),
                   SEC_CONFIRM_PENDING, "avant l'echeance, l'operation est en attente");
}

static void test_peek_on_idle_is_idle(void)
{
    sec_confirm_reset();
    TEST_ASSERT_EQ(sec_confirm_peek(50000), SEC_CONFIRM_IDLE,
                   "rien d'arme : rien a montrer");
}

/* L'ecran doit nommer ce qu'il fait confirmer. Un numero de slot ne le permet
 * pas : toutes les operations CCID partagent le meme slot.
 *
 * sec_confirm_armed_op() a existe puis a ete retiree en revue : deux
 * accesseurs separes (peek() + un lecteur d'operation) laissaient un appelant
 * lire l'etat et l'operation en deux appels non synchronises, ce qu'un
 * reset()+arm() intercale peut couper — l'ecran montrerait alors l'etat d'une
 * operation avec le libelle d'une autre. sec_confirm_peek_labeled() est le
 * seul accesseur desormais : un seul appel rend les deux, comme le contrat
 * l'exige (voir CONCURRENCY MODEL dans sec_confirm.c). */
static void test_armed_op_is_reported(void)
{
    sec_confirm_reset();
    sec_confirm_arm(0xF0u, SEC_OP_SIGN, 1000);
    sec_op_t op = SEC_OP_UNKNOWN;
    sec_confirm_peek_labeled(1000, &op);
    TEST_ASSERT_EQ(op, SEC_OP_SIGN, "l'operation armee est rendue");
}

static void test_armed_op_survives_peek(void)
{
    sec_confirm_reset();
    sec_confirm_arm(0xF0u, SEC_OP_DECRYPT, 1000);
    (void)sec_confirm_peek(1100);
    sec_op_t op = SEC_OP_UNKNOWN;
    sec_confirm_peek_labeled(1100, &op);
    TEST_ASSERT_EQ(op, SEC_OP_DECRYPT, "peek ne detruit pas l'operation");
}

/* Rien d'arme : l'ecran ne doit pas afficher l'operation PRECEDENTE, sinon il
 * ment sur ce qui se passe. */
static void test_reset_clears_the_op(void)
{
    sec_confirm_reset();
    sec_confirm_arm(0xF0u, SEC_OP_AUTH, 1000);
    sec_confirm_reset();
    sec_op_t op = SEC_OP_AUTH;   /* pollue volontairement : peek_labeled doit l'ecraser */
    sec_confirm_peek_labeled(1000, &op);
    TEST_ASSERT_EQ(op, SEC_OP_UNKNOWN, "apres reset, aucune operation n'est armee");
}

/* Deux armements successifs : c'est le dernier qui compte. */
static void test_rearm_replaces_the_op(void)
{
    sec_confirm_reset();
    sec_confirm_arm(0xF0u, SEC_OP_SIGN, 1000);
    sec_confirm_arm(0xF0u, SEC_OP_OTP, 2000);
    sec_op_t op = SEC_OP_UNKNOWN;
    sec_confirm_peek_labeled(2000, &op);
    TEST_ASSERT_EQ(op, SEC_OP_OTP, "le dernier armement gagne");
}

/* poll() consomme une autorisation ou acte une expiration : dans les deux cas
 * l'operation qu'elle nommait ne doit pas survivre a sa propre consommation
 * — sinon poll() et reset() divergent sur ce que IDLE veut dire, et le
 * prochain lecteur devrait re-deriver la reponse a la main. */
static void test_poll_clears_the_op_on_consume_or_timeout(void)
{
    sec_confirm_reset();
    sec_confirm_arm(0xF0u, SEC_OP_SIGN, 1000);
    sec_confirm_authorize(1050);
    TEST_ASSERT_EQ(sec_confirm_poll(1100, NULL), SEC_CONFIRM_AUTHORIZED,
                   "consommee par poll()");
    sec_op_t op = SEC_OP_SIGN;   /* pollue volontairement */
    sec_confirm_peek_labeled(1100, &op);
    TEST_ASSERT_EQ(op, SEC_OP_UNKNOWN, "poll() consommee efface aussi l'operation");

    sec_confirm_reset();
    sec_confirm_arm(0xF0u, SEC_OP_DECRYPT, 1000);
    TEST_ASSERT_EQ(sec_confirm_poll(1000 + SEC_CONFIRM_TIMEOUT_MS, NULL),
                   SEC_CONFIRM_TIMEDOUT, "expiree par poll()");
    op = SEC_OP_DECRYPT;         /* pollue volontairement */
    sec_confirm_peek_labeled(1000 + SEC_CONFIRM_TIMEOUT_MS, &op);
    TEST_ASSERT_EQ(op, SEC_OP_UNKNOWN, "poll() expiree efface aussi l'operation");
}

/* out_op == NULL est documente dans l'en-tete comme supporte : l'appelant qui
 * ne veut que l'etat (le meme role que peek()) ne doit pas etre force a
 * fournir un pointeur. Sans ce test, retirer le garde `if (out_op)` ne
 * ferait rougir aucun test existant — ils passent tous un pointeur valide. */
static void test_peek_labeled_tolerates_null_out_op(void)
{
    sec_confirm_reset();
    sec_confirm_arm(0xF0u, SEC_OP_SIGN, 1000);
    TEST_ASSERT_EQ(sec_confirm_peek_labeled(1000, NULL), SEC_CONFIRM_PENDING,
                   "out_op NULL : l'etat est quand meme rendu, rien ne deref NULL");
}


/* ------------------------------------------------------------------------- */
/* Ruling 28 — l'appui doit tomber DANS la fenetre de l'operation armee.      */
/* ------------------------------------------------------------------------- */

/*
 * Le scenario reel, et la raison d'etre de l'horodatage.
 *
 * hmi_task lit peek() puis appelle authorize() : deux appels separes, que rien
 * ne serialise contre la tache qui arme. Si l'ordonnanceur la laisse hors CPU
 * entre les deux — millisecondes, sous charge — l'operation A peut expirer et
 * une operation B s'armer dans l'intervalle. Sans horodatage, authorize() ne
 * voit qu'un s_state PENDING et accorde : l'appui destine a A autorise B.
 *
 * L'utilisateur a physiquement confirme quelque chose qu'on ne lui a jamais
 * montre. C'est exactement l'attaque que la porte de presence existe pour
 * fermer, et sans PIN dans le futur perimetre FIDO, cette porte est la SEULE
 * defense qui reste.
 */
static void test_a_press_meant_for_a_cannot_authorize_b(void)
{
    sec_confirm_reset();

    /* Operation A armee a t=1000. L'utilisateur appuie a t=1200. */
    sec_confirm_arm(1, SEC_OP_SIGN, 1000);
    const uint32_t pressed_for_a = 1200;

    /* hmi_task est preemptee ici. A expire, B s'arme a t=20000. */
    TEST_ASSERT_EQ(sec_confirm_poll(1000 + 15000, NULL), SEC_CONFIRM_TIMEDOUT,
                   "A expire faute d'appui relaye a temps");
    sec_confirm_arm(2, SEC_OP_DECRYPT, 20000);

    /* hmi_task reprend et relaie ENFIN l'appui, avec son horodatage d'origine. */
    sec_confirm_authorize(pressed_for_a);

    uint8_t slot = 0xFF;
    TEST_ASSERT_EQ(sec_confirm_poll(20100, &slot), SEC_CONFIRM_PENDING,
                   "B reste en attente : un appui anterieur a son armement ne l'autorise pas");
    TEST_ASSERT(slot == 0xFF, "aucun emplacement n'a ete accorde");
}

/* Un appui ANTERIEUR a l'armement courant est refuse, meme sans expiration
 * entre les deux : quelqu'un qui appuie en anticipant ne pre-autorise pas la
 * prochaine operation. */
static void test_press_before_arming_is_refused(void)
{
    sec_confirm_reset();
    sec_confirm_arm(3, SEC_OP_AUTH, 5000);
    sec_confirm_authorize(4999);   /* une milliseconde trop tot */
    TEST_ASSERT_EQ(sec_confirm_poll(5100, NULL), SEC_CONFIRM_PENDING,
                   "un appui d'avant l'armement n'accorde rien");
}

/* Et un appui trop TARD est refuse aussi : la meme expression borne les deux
 * cotes de la fenetre. */
static void test_press_after_the_window_is_refused(void)
{
    sec_confirm_reset();
    sec_confirm_arm(4, SEC_OP_OTP, 5000);
    sec_confirm_authorize(5000 + SEC_CONFIRM_TIMEOUT_MS);
    TEST_ASSERT_EQ(sec_confirm_poll(5000 + 100, NULL), SEC_CONFIRM_PENDING,
                   "un appui au seuil d'expiration n'accorde rien");
}

/* Les bornes exactes, comparees a des valeurs distinctes de part et d'autre —
 * pas une valeur a elle-meme. */
static void test_press_window_bounds_are_exact(void)
{
    /* Pile a l'instant de l'armement : accepte (zero milliseconde ecoulee). */
    sec_confirm_reset();
    sec_confirm_arm(1, SEC_OP_SIGN, 7000);
    sec_confirm_authorize(7000);
    TEST_ASSERT_EQ(sec_confirm_poll(7100, NULL), SEC_CONFIRM_AUTHORIZED,
                   "appui pile a l'armement : accorde");

    /* Derniere milliseconde valable : accepte. */
    sec_confirm_reset();
    sec_confirm_arm(1, SEC_OP_SIGN, 7000);
    sec_confirm_authorize(7000 + SEC_CONFIRM_TIMEOUT_MS - 1u);
    TEST_ASSERT_EQ(sec_confirm_poll(7100, NULL), SEC_CONFIRM_AUTHORIZED,
                   "appui a la derniere milliseconde : accorde");

    /* Une de plus : refuse. */
    sec_confirm_reset();
    sec_confirm_arm(1, SEC_OP_SIGN, 7000);
    sec_confirm_authorize(7000 + SEC_CONFIRM_TIMEOUT_MS);
    TEST_ASSERT_EQ(sec_confirm_poll(7100, NULL), SEC_CONFIRM_PENDING,
                   "une milliseconde de trop : refuse");
}

/* Le repassage a zero du compteur de millisecondes, apres ~49 jours : une cle
 * branchee en permanence l'atteint. Un appui legitime a cheval sur le
 * repassage doit rester accepte, sinon la cle deviendrait inutilisable une
 * fois tous les quarante-neuf jours. */
static void test_press_window_survives_millisecond_wraparound(void)
{
    const uint32_t armed = 0xFFFFFFFFu - 100u;

    sec_confirm_reset();
    sec_confirm_arm(1, SEC_OP_SIGN, armed);
    sec_confirm_authorize(armed + 200u);   /* repasse par zero entre les deux */
    TEST_ASSERT_EQ(sec_confirm_poll(armed + 300u, NULL), SEC_CONFIRM_AUTHORIZED,
                   "appui valable a cheval sur le repassage a zero : accorde");

    /* Et un appui anterieur reste refuse malgre le repassage. */
    sec_confirm_reset();
    sec_confirm_arm(1, SEC_OP_SIGN, armed);
    sec_confirm_authorize(armed - 1u);
    TEST_ASSERT_EQ(sec_confirm_poll(armed + 10u, NULL), SEC_CONFIRM_PENDING,
                   "appui anterieur, a cheval sur le repassage : refuse");
}

void test_sec_confirm(void)
{
    TEST_SUITE("sec_confirm state machine");
    TEST_RUN(test_a_press_meant_for_a_cannot_authorize_b);
    TEST_RUN(test_press_before_arming_is_refused);
    TEST_RUN(test_press_after_the_window_is_refused);
    TEST_RUN(test_press_window_bounds_are_exact);
    TEST_RUN(test_press_window_survives_millisecond_wraparound);
    TEST_RUN(test_arm_authorize_consume);
    TEST_RUN(test_timeout);
    TEST_RUN(test_authorize_without_arm);
    TEST_RUN(test_rearm_overwrites_slot);
    TEST_RUN(test_arm_while_authorized);
    TEST_RUN(test_peek_does_not_consume_the_grant);
    TEST_RUN(test_peek_reports_timeout_without_clearing_it);
    TEST_RUN(test_peek_sees_pending_before_timeout);
    TEST_RUN(test_peek_on_idle_is_idle);
    TEST_RUN(test_armed_op_is_reported);
    TEST_RUN(test_armed_op_survives_peek);
    TEST_RUN(test_reset_clears_the_op);
    TEST_RUN(test_rearm_replaces_the_op);
    TEST_RUN(test_poll_clears_the_op_on_consume_or_timeout);
    TEST_RUN(test_peek_labeled_tolerates_null_out_op);
}
