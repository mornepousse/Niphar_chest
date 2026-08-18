#define TEST_HOST 1
#include "test_framework.h"
#include "sec_store.h"

static void test_set_get(void)
{
    sec_store_init();
    uint8_t key[20] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20};
    TEST_ASSERT(sec_store_set_slot(1, SEC_SLOT_HMAC_SHA1, "github", key, 20), "set ok");
    TEST_ASSERT_EQ(sec_store_type(1), SEC_SLOT_HMAC_SHA1, "type stored");
    TEST_ASSERT(strcmp(sec_store_label(1), "github") == 0, "label stored");
    uint8_t out[64]; uint8_t olen = 0;
    TEST_ASSERT(sec_store_get_secret(1, out, &olen), "get secret ok");
    TEST_ASSERT_EQ(olen, 20, "secret_len");
    TEST_ASSERT(memcmp(out, key, 20) == 0, "secret bytes match");
    TEST_ASSERT_EQ(sec_store_count(), 1, "count = 1");
}

static void test_clear(void)
{
    sec_store_init();
    uint8_t key[4] = {9,9,9,9};
    sec_store_set_slot(0, SEC_SLOT_HMAC_SHA1, "x", key, 4);
    TEST_ASSERT(sec_store_clear_slot(0), "clear ok");
    TEST_ASSERT_EQ(sec_store_type(0), SEC_SLOT_EMPTY, "slot empty after clear");
    TEST_ASSERT(sec_store_label(0) == NULL, "label NULL after clear");
    TEST_ASSERT_EQ(sec_store_count(), 0, "count = 0");
}

static void test_bounds(void)
{
    sec_store_init();
    uint8_t key[4] = {1,2,3,4};
    TEST_ASSERT(!sec_store_set_slot(SEC_N_SLOTS, SEC_SLOT_HMAC_SHA1, "x", key, 4), "idx oob rejected");
    uint8_t big[65] = {0};
    TEST_ASSERT(!sec_store_set_slot(0, SEC_SLOT_HMAC_SHA1, "x", big, 65), "secret_len > max rejected");
    TEST_ASSERT(sec_store_get_secret(2, key, NULL) == false, "get empty slot fails");
    /* I-2: NULL secret with nonzero len rejected */
    TEST_ASSERT(!sec_store_set_slot(0, SEC_SLOT_HMAC_SHA1, "x", NULL, 4),
                "NULL secret + len>0 rejected");
    /* secret_len = 0 is valid (e.g. label-only slot) */
    TEST_ASSERT(sec_store_set_slot(0, SEC_SLOT_HMAC_SHA1, "x", NULL, 0),
                "secret_len=0 accepted");
    /* label = NULL handled */
    uint8_t k2[4] = {1,2,3,4};
    TEST_ASSERT(sec_store_set_slot(1, SEC_SLOT_HMAC_SHA1, NULL, k2, 4),
                "NULL label accepted");
    /* type = SEC_SLOT_EMPTY rejected (would persist an unreachable secret) */
    TEST_ASSERT(!sec_store_set_slot(2, SEC_SLOT_EMPTY, "x", k2, 4),
                "type=EMPTY rejected");
}

/* Seize slots, pas quatre : douze comptes TOTP plus de la marge. Le test
 * compare la borne HAUTE valide et la première INVALIDE — vérifier seulement
 * que le slot 15 marche laisserait passer un magasin de 64 slots. */
static void test_seize_slots_et_pas_un_de_plus(void)
{
    sec_store_init();
    const uint8_t key[4] = { 1, 2, 3, 4 };
    TEST_ASSERT(sec_store_set_slot(15, SEC_SLOT_HMAC_SHA1, "dernier", key, 4),
                "le slot 15 existe");
    TEST_ASSERT(!sec_store_set_slot(16, SEC_SLOT_HMAC_SHA1, "trop", key, 4),
                "le slot 16 n'existe pas");
}

/* Un nom YKOATH s'ecrit « Issuer:compte@domaine » : seize caracteres ne
 * suffisaient pas. Le test verifie qu'un nom long survit ENTIER, et qu'un nom
 * trop long est tronque avec son terminateur — pas qu'il deborde. */
static void test_nom_long_conserve(void)
{
    sec_store_init();
    const uint8_t key[4] = { 1, 2, 3, 4 };
    const char *nom = "GitHub:mae.pugin@exemple-tres-long.org";
    TEST_ASSERT(sec_store_set_slot(0, SEC_SLOT_HMAC_SHA1, nom, key, 4),
                "un nom de 38 caracteres est accepte");
    TEST_ASSERT(strcmp(sec_store_label(0), nom) == 0,
                "le nom long est conserve entier");

    char trop_long[128];
    memset(trop_long, 'x', sizeof(trop_long));
    trop_long[sizeof(trop_long) - 1] = '\0';
    TEST_ASSERT(sec_store_set_slot(1, SEC_SLOT_HMAC_SHA1, trop_long, key, 4),
                "un nom trop long est accepte, tronque");
    TEST_ASSERT(strlen(sec_store_label(1)) == SEC_LABEL_LEN - 1,
                "tronque a SEC_LABEL_LEN-1, terminateur compris");
}

/* Le nombre de chiffres est porte par le slot : six ou huit selon le compte.
 * Compare deux slots ENTRE EUX — verifier qu'un slot rend 6 ne prouve pas que
 * la valeur est lue du slot plutot que d'une constante. */
static void test_digits_par_slot(void)
{
    sec_store_init();
    const uint8_t key[4] = { 1, 2, 3, 4 };
    sec_store_set_slot(0, SEC_SLOT_HMAC_SHA1, "a", key, 4);
    sec_store_set_slot(1, SEC_SLOT_HMAC_SHA1, "b", key, 4);
    TEST_ASSERT(sec_store_set_digits(0, 6), "6 chiffres accepte");
    TEST_ASSERT(sec_store_set_digits(1, 8), "8 chiffres accepte");
    TEST_ASSERT(sec_store_digits(0) != sec_store_digits(1),
                "deux slots portent des valeurs distinctes");
    TEST_ASSERT_EQ(sec_store_digits(0), 6, "slot 0 rend bien 6");
    TEST_ASSERT_EQ(sec_store_digits(1), 8, "slot 1 rend bien 8");
    TEST_ASSERT(!sec_store_set_digits(0, 5), "5 chiffres refuse");
    TEST_ASSERT(!sec_store_set_digits(0, 9), "9 chiffres refuse");
}

void test_sec_store(void)
{
    TEST_SUITE("sec_store");
    TEST_RUN(test_set_get);
    TEST_RUN(test_clear);
    TEST_RUN(test_bounds);
    TEST_RUN(test_seize_slots_et_pas_un_de_plus);
    TEST_RUN(test_nom_long_conserve);
    TEST_RUN(test_digits_par_slot);
}
