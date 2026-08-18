/* Tests de ecdsa_der.c — encodage DER d'une signature ECDSA P-256 brute
 * (r||s -> SEQUENCE{ INTEGER r, INTEGER s }), verifie octet par octet contre
 * des valeurs calculees a la main, pas seulement "ca ne plante pas". */
#include "test_framework.h"
#include "ecdsa_der.h"
#include <string.h>

/* r et s "ronds", sans bit de signe pose ni zero de tete : le cas le plus
 * simple, chaque entier tient sur 1 octet de contenu. */
static void test_small_values_no_padding(void)
{
    uint8_t r[32]; memset(r, 0, 32); r[31] = 0x01;
    uint8_t s[32]; memset(s, 0, 32); s[31] = 0x02;
    uint8_t out[ECDSA_DER_MAX_LEN];

    size_t n = ecdsa_der_encode(r, s, out, sizeof(out));
    static const uint8_t expected[] = {
        0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x02
    };
    TEST_ASSERT_EQ(n, sizeof(expected), "longueur exacte, ni plus ni moins");
    TEST_ASSERT(memcmp(out, expected, sizeof(expected)) == 0,
               "octets DER exacts pour deux petits entiers");
}

/* r a son octet de poids fort >= 0x80 : un DER INTEGER sans bourrage serait
 * lu comme une valeur NEGATIVE, ce qui est faux pour ce r la (issu d'une
 * signature ECDSA, toujours positif). Le bourrage 0x00 doit apparaitre. */
static void test_high_bit_set_gets_padded(void)
{
    uint8_t r[32]; memset(r, 0, 32); r[31] = 0x80;
    uint8_t s[32]; memset(s, 0, 32); s[31] = 0x02;
    uint8_t out[ECDSA_DER_MAX_LEN];

    size_t n = ecdsa_der_encode(r, s, out, sizeof(out));
    static const uint8_t expected[] = {
        0x30, 0x07, 0x02, 0x02, 0x00, 0x80, 0x02, 0x01, 0x02
    };
    TEST_ASSERT_EQ(n, sizeof(expected), "un octet de bourrage allonge r de 1");
    TEST_ASSERT(memcmp(out, expected, sizeof(expected)) == 0,
               "le bourrage 0x00 precede bien l'octet 0x80");
}

/* r a plusieurs zeros de tete (issus d'un scalaire aleatoire dont les octets
 * de poids fort valent 0) : ils doivent disparaitre de l'encodage DER, qui
 * n'admet pas de zeros non significatifs en tete d'un INTEGER. */
static void test_leading_zero_bytes_are_stripped(void)
{
    uint8_t r[32]; memset(r, 0, 32); r[29] = 0x01; r[30] = 0x02; r[31] = 0x03;
    uint8_t s[32]; memset(s, 0, 32); s[31] = 0x05;
    uint8_t out[ECDSA_DER_MAX_LEN];

    size_t n = ecdsa_der_encode(r, s, out, sizeof(out));
    static const uint8_t expected[] = {
        0x30, 0x08, 0x02, 0x03, 0x01, 0x02, 0x03, 0x02, 0x01, 0x05
    };
    TEST_ASSERT_EQ(n, sizeof(expected), "r se reduit a ses 3 octets significatifs");
    TEST_ASSERT(memcmp(out, expected, sizeof(expected)) == 0,
               "aucun zero de tete ne survit dans le DER");
}

/* Cas maximal : r ET s valent 32 x 0xFF, le pire cas pour la taille (les deux
 * ont besoin d'un bourrage). Verifie que ECDSA_DER_MAX_LEN est un plafond
 * exact et pas une sous-estimation. */
static void test_max_size_case_fits_exactly(void)
{
    uint8_t r[32]; memset(r, 0xFF, 32);
    uint8_t s[32]; memset(s, 0xFF, 32);
    uint8_t out[ECDSA_DER_MAX_LEN];

    size_t n = ecdsa_der_encode(r, s, out, sizeof(out));
    TEST_ASSERT_EQ(n, ECDSA_DER_MAX_LEN, "le pire cas occupe exactement ECDSA_DER_MAX_LEN");
    TEST_ASSERT_EQ(out[0], 0x30, "tag SEQUENCE");
    TEST_ASSERT_EQ(out[1], ECDSA_DER_MAX_LEN - 2, "longueur du contenu de la SEQUENCE");
    /* Chaque INTEGER : tag 0x02, longueur 33 (bourrage + 32 x 0xFF) */
    TEST_ASSERT_EQ(out[2], 0x02, "tag INTEGER r");
    TEST_ASSERT_EQ(out[3], 33, "longueur de r avec bourrage");
    TEST_ASSERT_EQ(out[4], 0x00, "bourrage de r");
    TEST_ASSERT_EQ(out[2 + 2 + 33], 0x02, "tag INTEGER s");
    TEST_ASSERT_EQ(out[2 + 2 + 33 + 1], 33, "longueur de s avec bourrage");
}

/* r == 0 (32 octets nuls) est un cas degenere valide en DER : un seul octet
 * 0x00, ni zero de tete "en trop" ni bourrage (0x00 n'a pas le bit de signe
 * pose). */
static void test_zero_value_encodes_as_single_zero_byte(void)
{
    uint8_t r[32]; memset(r, 0, 32);
    uint8_t s[32]; memset(s, 0, 32); s[31] = 0x01;
    uint8_t out[ECDSA_DER_MAX_LEN];

    size_t n = ecdsa_der_encode(r, s, out, sizeof(out));
    static const uint8_t expected[] = {
        0x30, 0x06, 0x02, 0x01, 0x00, 0x02, 0x01, 0x01
    };
    TEST_ASSERT_EQ(n, sizeof(expected), "r=0 s'encode sur un seul octet 0x00");
    TEST_ASSERT(memcmp(out, expected, sizeof(expected)) == 0, "octets exacts pour r=0");
}

/* Un tampon de sortie trop petit doit echouer explicitement (0), jamais
 * ecrire une trame DER tronquee que l'appelant prendrait pour argent
 * comptant. */
static void test_buffer_too_small_fails_explicitly(void)
{
    uint8_t r[32]; memset(r, 0xFF, 32);
    uint8_t s[32]; memset(s, 0xFF, 32);
    uint8_t out[ECDSA_DER_MAX_LEN];

    size_t n = ecdsa_der_encode(r, s, out, ECDSA_DER_MAX_LEN - 1u);
    TEST_ASSERT_EQ(n, 0, "capacite insuffisante d'un seul octet : refus, pas troncature");
}

/* Un pointeur nul, sur n'importe quel argument, doit echouer explicitement —
 * jamais un dereferencement, jamais une ecriture partielle. */
static void test_null_arguments_fail_explicitly(void)
{
    uint8_t r[32]; memset(r, 0, 32);
    uint8_t s[32]; memset(s, 0, 32);
    uint8_t out[ECDSA_DER_MAX_LEN];

    TEST_ASSERT_EQ(ecdsa_der_encode(NULL, s, out, sizeof(out)), 0, "r nul refuse");
    TEST_ASSERT_EQ(ecdsa_der_encode(r, NULL, out, sizeof(out)), 0, "s nul refuse");
    TEST_ASSERT_EQ(ecdsa_der_encode(r, s, NULL, sizeof(out)), 0, "out nul refuse");
}

void test_ecdsa_der(void)
{
    TEST_SUITE("ecdsa_der — encodage DER d'une signature P-256");
    TEST_RUN(test_small_values_no_padding);
    TEST_RUN(test_high_bit_set_gets_padded);
    TEST_RUN(test_leading_zero_bytes_are_stripped);
    TEST_RUN(test_max_size_case_fits_exactly);
    TEST_RUN(test_zero_value_encodes_as_single_zero_byte);
    TEST_RUN(test_buffer_too_small_fails_explicitly);
    TEST_RUN(test_null_arguments_fail_explicitly);
}
