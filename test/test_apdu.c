#include "test_framework.h"
#include "apdu.h"
#include <string.h>

/* ---- case 3 short (from plan) ---- */
static void test_case3_short(void)
{
    /* 00 A4 04 00 06 D2 76 00 01 24 01  → SELECT, Lc=6, no Le */
    uint8_t b[] = {0x00,0xA4,0x04,0x00,0x06,0xD2,0x76,0x00,0x01,0x24,0x01};
    apdu_t a;
    TEST_ASSERT(apdu_parse(b, sizeof(b), &a), "case3 parses");
    TEST_ASSERT_EQ(a.ins, 0xA4, "ins");
    TEST_ASSERT_EQ(a.lc, 6, "lc");
    TEST_ASSERT_EQ(a.data[0], 0xD2, "data0");
    TEST_ASSERT_EQ(a.le, 0, "no le");
    TEST_ASSERT(!a.le_present, "le_present false");
}

/* ---- extended Le (from plan) ---- */
static void test_extended_le(void)
{
    /* 00 CA 00 6E 00 00 00 → GET DATA, extended Le=0 (=65536) */
    uint8_t b[] = {0x00,0xCA,0x00,0x6E,0x00,0x00,0x00};
    apdu_t a;
    TEST_ASSERT(apdu_parse(b, sizeof(b), &a), "ext Le parses");
    TEST_ASSERT_EQ(a.lc, 0, "no lc");
    TEST_ASSERT(a.le_present, "le present");
}

/* ---- case 1: header only, no Lc, no Le ---- */
static void test_case1_no_data(void)
{
    /* VERIFY check-only: 00 20 00 82 (4 bytes) */
    uint8_t b[] = {0x00, 0x20, 0x00, 0x82};
    apdu_t a;
    TEST_ASSERT(apdu_parse(b, sizeof(b), &a), "case1 parses");
    TEST_ASSERT_EQ(a.cla, 0x00, "cla");
    TEST_ASSERT_EQ(a.ins, 0x20, "ins");
    TEST_ASSERT_EQ(a.p2,  0x82, "p2");
    TEST_ASSERT_EQ(a.lc,  0,    "no lc");
    TEST_ASSERT(!a.le_present,  "no le");
}

/* ---- case 2 short: Le only (non-zero) ---- */
static void test_case2_le_only(void)
{
    /* GET DATA: 00 CA 00 6E 10 → Le=0x10 */
    uint8_t b[] = {0x00, 0xCA, 0x00, 0x6E, 0x10};
    apdu_t a;
    TEST_ASSERT(apdu_parse(b, sizeof(b), &a), "case2 short Le parses");
    TEST_ASSERT_EQ(a.lc, 0, "no lc");
    TEST_ASSERT(a.le_present, "le present");
    TEST_ASSERT_EQ(a.le, 0x10, "le value");
}

/* ---- case 4 short: Lc + data + Le ---- */
static void test_case4_lc_le(void)
{
    /* VERIFY: 00 20 00 82 06 '1'..'6' 00 → Lc=6 data Le=0 */
    uint8_t b[] = {0x00,0x20,0x00,0x82, 0x06, '1','2','3','4','5','6', 0x00};
    apdu_t a;
    TEST_ASSERT(apdu_parse(b, sizeof(b), &a), "case4 short parses");
    TEST_ASSERT_EQ(a.ins, 0x20, "ins");
    TEST_ASSERT_EQ(a.lc,  6,    "lc");
    TEST_ASSERT_EQ(a.data[0], '1', "data0");
    TEST_ASSERT(a.le_present, "le present");
    TEST_ASSERT_EQ(a.le, 0x00, "le=0 (means 256 in ISO short form)");
}

/* ---- malformed: too short and truncated data ---- */
static void test_malformed_truncated(void)
{
    apdu_t a;

    /* Only 3 bytes: below minimum */
    uint8_t b1[] = {0x00, 0xA4, 0x04};
    TEST_ASSERT(!apdu_parse(b1, sizeof(b1), &a), "3-byte APDU rejected");

    /* Lc=6 but only 2 bytes of data supplied */
    uint8_t b2[] = {0x00,0xA4,0x04,0x00,0x06,0xD2,0x76};
    TEST_ASSERT(!apdu_parse(b2, sizeof(b2), &a), "truncated data rejected");

    /* NULL buffer */
    TEST_ASSERT(!apdu_parse(NULL, 4, &a), "NULL buf rejected");

    /* NULL output */
    uint8_t b3[] = {0x00,0xA4,0x04,0x00};
    TEST_ASSERT(!apdu_parse(b3, sizeof(b3), NULL), "NULL out rejected");
}

/*
 * ---- lacune de couverture amont : troncature en format ÉTENDU ----
 *
 * test_malformed_truncated (ci-dessus, hérité de KeSp) ne teste la troncature
 * qu'en format COURT (Lc sur 1 octet). Le format étendu (buf[4]==0x00,
 * len>=7, Lc sur les octets 5-6) a son propre contrôle de troncature en
 * apdu.c:37 — `if ((uint32_t)len < data_end) return false;` — et rien dans
 * la suite portée ne l'exerce. Vérifié par mutation testing (tâche 4) :
 * neutraliser cette ligne laisse les 377 assertions d'origine vertes.
 *
 * Pourquoi ça compte : `apdu_parse` décode des octets envoyés par un hôte
 * USB non fiable. Sans ce contrôle, une APDU étendue qui annonce un Lc plus
 * grand que ce que le tampon contient réellement voit quand même out->lc et
 * out->data écrits (avec la valeur de Lc annoncée par l'attaquant, et un
 * pointeur dans le tampon) avant que la fonction ne retombe sur le rejet
 * générique de fin de fonction. Un appelant qui lirait out->lc octets depuis
 * out->data sur la seule foi de ces champs — au lieu du booléen de retour —
 * lirait au-delà du tampon fourni par l'hôte. Ces deux assertions vérifient
 * que sur troncature étendue, out->lc et out->data restent à leur valeur
 * memset(0) initiale et ne portent jamais la valeur annoncée par l'attaquant.
 */
static void test_extended_truncated_gap(void)
{
    apdu_t a;

    /* Lc annoncé = 1000 (0x03E8), mais seulement 2 octets de données
     * fournis après l'en-tête étendu de 7 octets : data_end = 7+1000 = 1007,
     * len = 9. Largement tronqué. */
    uint8_t big[] = {0x00, 0xA4, 0x04, 0x00, 0x00, 0x03, 0xE8, 0xFF, 0xFF};
    TEST_ASSERT(!apdu_parse(big, sizeof(big), &a),
                "extended Lc=1000, 2 bytes fournis : rejeté");
    TEST_ASSERT_EQ(a.lc, 0,
                "extended tronqué (large) : lc reste 0, ne fuit pas le Lc annoncé");
    TEST_ASSERT(a.data == NULL,
                "extended tronqué (large) : data reste NULL, pas de pointeur exposé");

    /* Même chemin, à la limite : Lc=2 (data_end=9) mais len=8, tronqué d'un
     * seul octet. Le contrôle doit mordre même à la marge, pas seulement sur
     * une troncature grossière. */
    uint8_t off_by_one[] = {0x00, 0xA4, 0x04, 0x00, 0x00, 0x00, 0x02, 0xFF};
    TEST_ASSERT(!apdu_parse(off_by_one, sizeof(off_by_one), &a),
                "extended Lc=2, 1 byte fourni sur 2 : rejeté (tronqué d'1 octet)");
    TEST_ASSERT_EQ(a.lc, 0,
                "extended tronqué (1 octet) : lc reste 0");
    TEST_ASSERT(a.data == NULL,
                "extended tronqué (1 octet) : data reste NULL");
}

void test_apdu(void)
{
    TEST_SUITE("apdu parser");
    TEST_RUN(test_case3_short);
    TEST_RUN(test_extended_le);
    TEST_RUN(test_case1_no_data);
    TEST_RUN(test_case2_le_only);
    TEST_RUN(test_case4_lc_le);
    TEST_RUN(test_malformed_truncated);
    TEST_RUN(test_extended_truncated_gap);
}
