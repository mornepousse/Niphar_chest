/* Tests du protocole du lien S3↔coffre.
 *
 * Écrits avant l'implémentation (norme TDD du CLAUDE.md). Ce qu'ils couvrent
 * est précisément ce qui peut être faux sans être visible : la détection
 * d'absence, l'ordre des octets, et le rejet de ce qui ne doit pas être
 * interprété.
 */
#include "test_framework.h"

#include "link/link_proto.h"

/* ------------------------------------------------------------------------ */
/* Aller-retour de la carte de registres                                     */
/* ------------------------------------------------------------------------ */

static void test_pack_parse_roundtrip(void)
{
    uint8_t regs[LINK_REG_SIZE];
    const link_status_t in = {
        .state = LINK_STATE_SD_PRESENT | LINK_STATE_READY,
        .pending_op = 0x1234,
        .confirm_count = 0x89ABCDEF,
    };

    link_proto_pack_status(regs, &in);

    link_status_t out;
    memset(&out, 0, sizeof(out));
    TEST_ASSERT(link_proto_parse_status(regs, &out), "bloc valide accepté");
    TEST_ASSERT_EQ(out.version, LINK_PROTO_VERSION, "version renvoyée");
    TEST_ASSERT_EQ(out.state, in.state, "état conservé");
    TEST_ASSERT_EQ(out.pending_op, in.pending_op, "opération en attente conservée");
    TEST_ASSERT_EQ(out.confirm_count, in.confirm_count, "compteur conservé");
}

/* Le compteur est sur 4 octets : une erreur d'ordre ne se voit qu'aux valeurs
 * asymétriques et aux extrêmes. */
static void test_confirm_count_full_range(void)
{
    uint8_t regs[LINK_REG_SIZE];
    link_status_t in = { .state = 0, .pending_op = 0, .confirm_count = 0xFFFFFFFFu };
    link_status_t out;

    link_proto_pack_status(regs, &in);
    TEST_ASSERT(link_proto_parse_status(regs, &out), "compteur au maximum accepté");
    TEST_ASSERT_EQ(out.confirm_count, 0xFFFFFFFFu, "compteur au maximum conservé");

    in.confirm_count = 1;
    link_proto_pack_status(regs, &in);
    TEST_ASSERT(link_proto_parse_status(regs, &out), "compteur à 1 accepté");
    TEST_ASSERT_EQ(out.confirm_count, 1, "compteur à 1 conservé");
}

static void test_state_bits_independent(void)
{
    uint8_t regs[LINK_REG_SIZE];
    link_status_t out;

    const uint8_t bits[] = { LINK_STATE_SD_PRESENT, LINK_STATE_USB_MOUNTED, LINK_STATE_READY };
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        link_status_t in = { .state = bits[i], .pending_op = 0, .confirm_count = 0 };
        link_proto_pack_status(regs, &in);
        TEST_ASSERT(link_proto_parse_status(regs, &out), "bloc à un seul bit accepté");
        TEST_ASSERT_EQ(out.state, bits[i], "bit d'état isolé conservé");
    }
}

/* ------------------------------------------------------------------------ */
/* Rejets                                                                    */
/* ------------------------------------------------------------------------ */

static void test_reject_bad_magic(void)
{
    uint8_t regs[LINK_REG_SIZE];
    const link_status_t in = { .state = 0, .pending_op = 0, .confirm_count = 0 };
    link_status_t out;

    link_proto_pack_status(regs, &in);
    regs[0] ^= 0xFF;   /* un seul octet du mot magique */
    TEST_ASSERT(!link_proto_parse_status(regs, &out), "mot magique faux rejeté");
}

static void test_reject_bad_version(void)
{
    uint8_t regs[LINK_REG_SIZE];
    const link_status_t in = { .state = 0, .pending_op = 0, .confirm_count = 0 };
    link_status_t out;

    link_proto_pack_status(regs, &in);
    regs[LINK_REG_VERSION] = LINK_PROTO_VERSION + 1;
    TEST_ASSERT(!link_proto_parse_status(regs, &out),
                "version inconnue rejetée plutôt qu'interprétée");
}

/* Le CRC est ce qui distingue une lecture SPI corrompue d'un état plausible :
 * le SPI ne fournit aucune détection d'erreur. */
static void test_reject_corrupted_payload(void)
{
    uint8_t regs[LINK_REG_SIZE];
    const link_status_t in = {
        .state = LINK_STATE_READY,
        .pending_op = 0x0042,
        .confirm_count = 7,
    };
    link_status_t out;

    link_proto_pack_status(regs, &in);
    regs[LINK_REG_CONFIRM_COUNT] ^= 0x01;   /* un bit, ailleurs que dans le magique */
    TEST_ASSERT(!link_proto_parse_status(regs, &out), "bit retourné détecté par le CRC");
}

/* La confirmation vient du S3 : elle est hors de la zone couverte par le CRC du
 * coffre, sinon toute écriture du maître invaliderait le bloc. */
static void test_user_confirm_outside_crc(void)
{
    uint8_t regs[LINK_REG_SIZE];
    const link_status_t in = { .state = 0, .pending_op = 0, .confirm_count = 0 };
    link_status_t out;

    link_proto_pack_status(regs, &in);
    regs[LINK_REG_USER_CONFIRM] = LINK_USER_CONFIRM_MAGIC;
    TEST_ASSERT(link_proto_parse_status(regs, &out),
                "écriture du maître n'invalide pas le CRC du coffre");
}

/* ------------------------------------------------------------------------ */
/* Absence — le cas normal, et celui qu'on code à l'envers                    */
/* ------------------------------------------------------------------------ */

static void test_absent_all_zero(void)
{
    uint8_t regs[LINK_REG_SIZE];
    memset(regs, 0x00, sizeof(regs));
    TEST_ASSERT(link_proto_is_absent(regs, sizeof(regs)), "tout à 0x00 = absent");

    link_status_t out;
    TEST_ASSERT(!link_proto_parse_status(regs, &out), "bloc absent non interprété");
}

static void test_absent_all_ones(void)
{
    uint8_t regs[LINK_REG_SIZE];
    memset(regs, 0xFF, sizeof(regs));
    TEST_ASSERT(link_proto_is_absent(regs, sizeof(regs)), "tout à 0xFF = absent");

    link_status_t out;
    TEST_ASSERT(!link_proto_parse_status(regs, &out), "bloc absent non interprété");
}

static void test_present_not_absent(void)
{
    uint8_t regs[LINK_REG_SIZE];
    const link_status_t in = { .state = 0, .pending_op = 0, .confirm_count = 0 };

    link_proto_pack_status(regs, &in);
    TEST_ASSERT(!link_proto_is_absent(regs, sizeof(regs)),
                "un coffre à l'état nul n'est pas un coffre absent");
}

/* Un bloc presque uniforme ne doit pas être confondu avec une ligne flottante. */
static void test_almost_uniform_is_present(void)
{
    uint8_t regs[LINK_REG_SIZE];
    memset(regs, 0xFF, sizeof(regs));
    regs[LINK_REG_SIZE - 1] = 0x00;
    TEST_ASSERT(!link_proto_is_absent(regs, sizeof(regs)),
                "un seul octet différent suffit à écarter l'absence");
}

/* ------------------------------------------------------------------------ */

void test_link_proto(void)
{
    TEST_SUITE("link_proto");
    TEST_RUN(test_pack_parse_roundtrip);
    TEST_RUN(test_confirm_count_full_range);
    TEST_RUN(test_state_bits_independent);
    TEST_RUN(test_reject_bad_magic);
    TEST_RUN(test_reject_bad_version);
    TEST_RUN(test_reject_corrupted_payload);
    TEST_RUN(test_user_confirm_outside_crc);
    TEST_RUN(test_absent_all_zero);
    TEST_RUN(test_absent_all_ones);
    TEST_RUN(test_present_not_absent);
    TEST_RUN(test_almost_uniform_is_present);
}
