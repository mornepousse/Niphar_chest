#include "test_framework.h"
#include "oath_proto.h"

/*
 * Vecteurs de la RFC 4226, annexe D : secret ASCII « 12345678901234567890 ».
 * DEUX compteurs, pas un — un seul vecteur ne distingue pas une troncature
 * juste d'une constante heureuse. On verifie le code DYNAMIQUE, pas les six
 * chiffres : la carte ne fait pas le modulo, ykman s'en charge (_format_code,
 * oath.py:259).
 *
 * Valeurs RECALCULEES, pas recopiees de memoire :
 *   compteur 0 -> hmac cc93cf...e4b0, dbc 1284755224, code 755224
 *   compteur 1 -> hmac 75a48a...33ab, dbc 1094287082, code 287082
 * Le plan avait initialement apparie le hmac du compteur 1 au code du
 * compteur 0 : le test aurait echoue et fait « corriger » du code juste.
 */
static void test_troncature_rfc4226(void)
{
    const uint8_t hmac0[20] = {
        0xcc,0x93,0xcf,0x18,0x50,0x8d,0x94,0x93,0x4c,0x64,
        0xb6,0x5d,0x8b,0xa7,0x66,0x7f,0xb7,0xcd,0xe4,0xb0
    };
    const uint8_t hmac1[20] = {
        0x75,0xa4,0x8a,0x19,0xd4,0xcb,0xe1,0x00,0x64,0x4e,
        0x8a,0xc1,0x39,0x7e,0xea,0x74,0x7a,0x2d,0x33,0xab
    };
    const uint32_t d0 = oath_dynamic_binary(hmac0, sizeof(hmac0));
    const uint32_t d1 = oath_dynamic_binary(hmac1, sizeof(hmac1));

    TEST_ASSERT_EQ(d0, 1284755224u, "vecteur RFC 4226 compteur 0 — code dynamique");
    TEST_ASSERT_EQ(d1, 1094287082u, "vecteur RFC 4226 compteur 1 — code dynamique");
    TEST_ASSERT_EQ(d0 % 1000000u, 755224u, "compteur 0 -> 755224 apres modulo hote");
    TEST_ASSERT_EQ(d1 % 1000000u, 287082u, "compteur 1 -> 287082 apres modulo hote");
    TEST_ASSERT(d0 != d1, "deux compteurs donnent deux codes distincts");
    TEST_ASSERT((d0 & 0x80000000u) == 0, "le bit de poids fort est masque");
    TEST_ASSERT((d1 & 0x80000000u) == 0, "le bit de poids fort est masque");
}

/* L'offset vient du dernier quartet : deux HMAC differant SEULEMENT par ce
 * quartet doivent produire des codes differents. Verifier un seul vecteur ne
 * prouverait pas que l'offset est lu. */
static void test_offset_lu_du_dernier_quartet(void)
{
    uint8_t h[20];
    for (unsigned i = 0; i < sizeof(h); i++) h[i] = (uint8_t)i;
    h[19] = 0x00;
    uint32_t a = oath_dynamic_binary(h, sizeof(h));
    h[19] = 0x05;
    uint32_t b = oath_dynamic_binary(h, sizeof(h));
    TEST_ASSERT(a != b, "changer l'offset change le code");
}

/* Un TLV absent doit se dire absent, pas rendre le TLV voisin. */
static void test_tlv_trouve_et_absent(void)
{
    const uint8_t buf[] = { 0x71, 0x03, 'a','b','c', 0x74, 0x02, 0xAA, 0xBB };
    const uint8_t *v = NULL; uint16_t n = 0;
    TEST_ASSERT(oath_tlv_find(buf, sizeof(buf), 0x74, &v, &n), "0x74 present");
    TEST_ASSERT_EQ(n, 2, "longueur du 0x74");
    TEST_ASSERT(v[0] == 0xAA && v[1] == 0xBB, "valeur du 0x74");
    TEST_ASSERT(!oath_tlv_find(buf, sizeof(buf), 0x79, &v, &n), "0x79 absent");
}

/* Un TLV dont la longueur deborde du tampon est une trame malformee venue de
 * l'hote : elle doit etre refusee, pas lue au-dela. */
static void test_tlv_longueur_qui_deborde(void)
{
    const uint8_t buf[] = { 0x71, 0x40, 'a', 'b' };
    const uint8_t *v = NULL; uint16_t n = 0;
    TEST_ASSERT(!oath_tlv_find(buf, sizeof(buf), 0x71, &v, &n),
                "une longueur qui deborde est refusee");
}

/* Ecriture : refuser plutot que deborder quand la capacite manque. */
static void test_tlv_put_borne(void)
{
    uint8_t out[4];
    const uint8_t v[8] = { 0 };
    TEST_ASSERT_EQ(oath_tlv_put(out, sizeof(out), 0x75, v, 8), 0,
                   "capacite insuffisante -> 0, rien d'ecrit");
    TEST_ASSERT_EQ(oath_tlv_put(out, sizeof(out), 0x75, v, 2), 4,
                   "tag + longueur + 2 octets = 4");
}

void test_oath_proto(void)
{
    TEST_SUITE("oath_proto");
    TEST_RUN(test_troncature_rfc4226);
    TEST_RUN(test_offset_lu_du_dernier_quartet);
    TEST_RUN(test_tlv_trouve_et_absent);
    TEST_RUN(test_tlv_longueur_qui_deborde);
    TEST_RUN(test_tlv_put_borne);
}
