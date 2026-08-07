/* Tests de la découpe d'un transfert MSC.
 *
 * C'est la seule arithmétique du projet pilotée directement par un hôte USB non
 * digne de confiance : un LBA et un décalage arbitraires, une longueur
 * arbitraire. Les cas de rejet comptent donc autant que les cas nominaux.
 */
#include "test_framework.h"

#include "usb/msc_lba.h"

#define SS      512u        /* taille de secteur usuelle */
#define COUNT   7626752u    /* la SE04G du banc : ~3,64 Gio */

/* ------------------------------------------------------------------------ */
/* Rejets — ce qui ne doit jamais atteindre la carte                         */
/* ------------------------------------------------------------------------ */

static void test_reject_zero_length(void)
{
    msc_lba_iter_t it;
    TEST_ASSERT(!msc_lba_begin(&it, 0, 0, 0, SS, COUNT, true),
                "longueur nulle rejetée");
}

static void test_reject_zero_sector_size(void)
{
    msc_lba_iter_t it;
    TEST_ASSERT(!msc_lba_begin(&it, 0, 0, SS, 0, COUNT, true),
                "taille de secteur nulle rejetée");
}

static void test_reject_no_medium(void)
{
    msc_lba_iter_t it;
    TEST_ASSERT(!msc_lba_begin(&it, 0, 0, SS, SS, 0, true),
                "carte de capacité nulle rejetée");
}

static void test_reject_past_end(void)
{
    msc_lba_iter_t it;
    /* Un octet de trop sur le dernier secteur. */
    TEST_ASSERT(!msc_lba_begin(&it, COUNT - 1, 1, SS, SS, COUNT, true),
                "dépassement d'un octet rejeté");
    TEST_ASSERT(!msc_lba_begin(&it, COUNT, 0, SS, SS, COUNT, true),
                "premier secteur hors capacité rejeté");
}

/* Le cœur du sujet : lba * sector_size déborde 32 bits dès 4 Gio. Un contrôle
 * de bornes fait sur 32 bits laisserait passer cet accès. */
static void test_reject_overflow(void)
{
    msc_lba_iter_t it;
    TEST_ASSERT(!msc_lba_begin(&it, 0xFFFFFFFFu, 0, SS, SS, COUNT, true),
                "LBA maximal ne déborde pas en un accès valide");
    TEST_ASSERT(!msc_lba_begin(&it, 0x00800000u, 0, SS, SS, COUNT, true),
                "LBA au-delà de 4 Gio rejeté");
    TEST_ASSERT(!msc_lba_begin(&it, COUNT - 1, 0xFFFFFF00u, SS, SS, COUNT, true),
                "décalage énorme rejeté sans repliement");
}

/* ------------------------------------------------------------------------ */
/* Acceptations aux bornes                                                   */
/* ------------------------------------------------------------------------ */

static void test_accept_exact_end(void)
{
    msc_lba_iter_t it;
    TEST_ASSERT(msc_lba_begin(&it, COUNT - 1, 0, SS, SS, COUNT, true),
                "dernier secteur entier accepté");
}

static void test_accept_first_sector(void)
{
    msc_lba_iter_t it;
    msc_span_t sp;

    TEST_ASSERT(msc_lba_begin(&it, 0, 0, SS, SS, COUNT, true), "secteur 0 accepté");
    TEST_ASSERT(msc_lba_next(&it, &sp), "un morceau");
    TEST_ASSERT_EQ(sp.sector, 0, "secteur 0");
    TEST_ASSERT_EQ(sp.in_sector, 0, "aligné");
    TEST_ASSERT_EQ(sp.bytes, SS, "un secteur entier");
    TEST_ASSERT_EQ(sp.sectors, 1, "compté comme un secteur entier");
    TEST_ASSERT(!msc_lba_next(&it, &sp), "puis terminé");
}

/* ------------------------------------------------------------------------ */
/* Découpe                                                                   */
/* ------------------------------------------------------------------------ */

/* Cas dominant en pratique : TinyUSB demande 32 Kio alignés. */
static void test_aligned_multi_single_span(void)
{
    msc_lba_iter_t it;
    msc_span_t sp;

    TEST_ASSERT(msc_lba_begin(&it, 100, 0, 64 * SS, SS, COUNT, true), "32 Kio alignés");
    TEST_ASSERT(msc_lba_next(&it, &sp), "un morceau");
    TEST_ASSERT_EQ(sp.sector, 100, "au bon secteur");
    TEST_ASSERT_EQ(sp.in_sector, 0, "aligné");
    TEST_ASSERT_EQ(sp.sectors, 64, "64 secteurs d'un coup");
    TEST_ASSERT_EQ(sp.bytes, 64 * SS, "tous les octets");
    TEST_ASSERT(!msc_lba_next(&it, &sp), "un seul morceau suffit");
}

/* Le même transfert quand le tampon n'est pas accessible au DMA : le découpage
 * doit couvrir exactement les mêmes octets, un secteur à la fois. */
static void test_aligned_single_sector_spans(void)
{
    msc_lba_iter_t it;
    msc_span_t sp;
    uint32_t total = 0, n = 0;

    TEST_ASSERT(msc_lba_begin(&it, 100, 0, 64 * SS, SS, COUNT, false), "sans multi-secteur");
    while (msc_lba_next(&it, &sp)) {
        TEST_ASSERT_EQ(sp.sector, 100 + n, "secteurs consécutifs");
        TEST_ASSERT_EQ(sp.in_sector, 0, "toujours aligné");
        TEST_ASSERT_EQ(sp.bytes, SS, "un secteur par morceau");
        total += sp.bytes;
        n++;
    }
    TEST_ASSERT_EQ(n, 64, "64 morceaux");
    TEST_ASSERT_EQ(total, 64 * SS, "mêmes octets couverts");
}

/* Décalage non aligné : c'est là que se logent les erreurs d'un secteur. */
static void test_unaligned_offset(void)
{
    msc_lba_iter_t it;
    msc_span_t sp;

    TEST_ASSERT(msc_lba_begin(&it, 10, 100, SS, SS, COUNT, true), "512 o à partir de +100");

    TEST_ASSERT(msc_lba_next(&it, &sp), "premier morceau");
    TEST_ASSERT_EQ(sp.sector, 10, "secteur 10");
    TEST_ASSERT_EQ(sp.in_sector, 100, "décalage conservé");
    TEST_ASSERT_EQ(sp.bytes, SS - 100, "fin du secteur seulement");
    TEST_ASSERT_EQ(sp.sectors, 0, "accès partiel, pas un secteur entier");

    TEST_ASSERT(msc_lba_next(&it, &sp), "second morceau");
    TEST_ASSERT_EQ(sp.sector, 11, "secteur suivant");
    TEST_ASSERT_EQ(sp.in_sector, 0, "aligné");
    TEST_ASSERT_EQ(sp.bytes, 100, "le reste");
    TEST_ASSERT_EQ(sp.sectors, 0, "accès partiel");

    TEST_ASSERT(!msc_lba_next(&it, &sp), "terminé");
}

/* Un décalage supérieur à un secteur doit se reporter sur le secteur suivant,
 * pas être tronqué. */
static void test_offset_beyond_one_sector(void)
{
    msc_lba_iter_t it;
    msc_span_t sp;

    TEST_ASSERT(msc_lba_begin(&it, 10, 3 * SS, SS, SS, COUNT, true), "décalage de 3 secteurs");
    TEST_ASSERT(msc_lba_next(&it, &sp), "un morceau");
    TEST_ASSERT_EQ(sp.sector, 13, "reporté sur le secteur 13");
    TEST_ASSERT_EQ(sp.in_sector, 0, "aligné");
}

/* Longueur qui n'est pas un multiple de la taille de secteur. */
static void test_partial_tail(void)
{
    msc_lba_iter_t it;
    msc_span_t sp;
    uint32_t total = 0;

    TEST_ASSERT(msc_lba_begin(&it, 0, 0, 700, SS, COUNT, true), "700 octets");

    TEST_ASSERT(msc_lba_next(&it, &sp), "premier morceau");
    TEST_ASSERT_EQ(sp.bytes, SS, "un secteur entier d'abord");
    TEST_ASSERT_EQ(sp.sectors, 1, "compté entier");
    total += sp.bytes;

    TEST_ASSERT(msc_lba_next(&it, &sp), "second morceau");
    TEST_ASSERT_EQ(sp.sector, 1, "secteur 1");
    TEST_ASSERT_EQ(sp.in_sector, 0, "aligné");
    TEST_ASSERT_EQ(sp.bytes, 700 - SS, "la queue");
    TEST_ASSERT_EQ(sp.sectors, 0, "queue partielle");
    total += sp.bytes;

    TEST_ASSERT(!msc_lba_next(&it, &sp), "terminé");
    TEST_ASSERT_EQ(total, 700, "tous les octets couverts, aucun en trop");
}

/* Invariant global : quelle que soit la découpe, la somme des morceaux vaut
 * exactement la longueur demandée et les adresses se suivent sans trou. */
static void test_spans_are_contiguous_and_exact(void)
{
    const uint32_t cases[][4] = {
        /* lba, offset, bufsize, allow_multi */
        {   0,    0,  4096, 1 },
        {   0,    1,  4096, 1 },
        {  42,  511,     2, 1 },
        {  42,  511,     2, 0 },
        { 999,  300,  1000, 1 },
        { 999,  300,  1000, 0 },
        {   7,    0,     1, 1 },
    };

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        msc_lba_iter_t it;
        msc_span_t sp;
        const uint32_t lba = cases[c][0], off = cases[c][1], len = cases[c][2];
        const bool multi = cases[c][3] != 0;

        TEST_ASSERT(msc_lba_begin(&it, lba, off, len, SS, COUNT, multi), "cas accepté");

        uint64_t expected = (uint64_t)lba * SS + off;
        uint32_t total = 0;
        while (msc_lba_next(&it, &sp)) {
            const uint64_t got = (uint64_t)sp.sector * SS + sp.in_sector;
            TEST_ASSERT(got == expected, "morceaux contigus, sans trou ni recouvrement");
            TEST_ASSERT(sp.bytes > 0, "morceau non vide");
            expected += sp.bytes;
            total += sp.bytes;
        }
        TEST_ASSERT_EQ(total, len, "somme des morceaux = longueur demandée");
    }
}

/* ------------------------------------------------------------------------ */

void test_msc_lba(void)
{
    TEST_SUITE("msc_lba");
    TEST_RUN(test_reject_zero_length);
    TEST_RUN(test_reject_zero_sector_size);
    TEST_RUN(test_reject_no_medium);
    TEST_RUN(test_reject_past_end);
    TEST_RUN(test_reject_overflow);
    TEST_RUN(test_accept_exact_end);
    TEST_RUN(test_accept_first_sector);
    TEST_RUN(test_aligned_multi_single_span);
    TEST_RUN(test_aligned_single_sector_spans);
    TEST_RUN(test_unaligned_offset);
    TEST_RUN(test_offset_beyond_one_sector);
    TEST_RUN(test_partial_tail);
    TEST_RUN(test_spans_are_contiguous_and_exact);
}
