#include "test_framework.h"
#include "cbor_enc.h"
#include <string.h>

/* ---- les trois tests du cahier des charges, repris verbatim ---- */

static void test_uint_uses_the_shortest_form(void)
{
    /* Encodage CANONIQUE : la plus courte forme possible. Un encodeur qui
     * emettrait toujours 8 octets serait valide en CBOR mais REFUSE par les
     * clients CTAP2, qui exigent le canonique. */
    uint8_t b[16]; cbor_w_t w;
    cbor_w_init(&w,b,sizeof b); cbor_uint(&w,0);
    TEST_ASSERT_EQ(w.len,1,"0 tient sur un octet"); TEST_ASSERT_EQ(b[0],0x00,"0x00");

    cbor_w_init(&w,b,sizeof b); cbor_uint(&w,23);
    TEST_ASSERT_EQ(w.len,1,"23 est la derniere valeur immediate");
    TEST_ASSERT_EQ(b[0],0x17,"0x17");

    cbor_w_init(&w,b,sizeof b); cbor_uint(&w,24);
    TEST_ASSERT_EQ(w.len,2,"24 bascule sur un octet suivant");
    TEST_ASSERT_EQ(b[0],0x18,"0x18"); TEST_ASSERT_EQ(b[1],24,"24");

    cbor_w_init(&w,b,sizeof b); cbor_uint(&w,255);
    TEST_ASSERT_EQ(w.len,2,"255 tient encore sur un octet suivant");
    cbor_w_init(&w,b,sizeof b); cbor_uint(&w,256);
    TEST_ASSERT_EQ(w.len,3,"256 exige deux octets");
    TEST_ASSERT_EQ(b[0],0x19,"0x19");
}

/* Un debordement doit se SIGNALER, jamais s'ecrire hors du tampon ni se
 * tronquer en silence : un CBOR tronque est un CBOR valide mais faux. */
static void test_overflow_is_flagged_not_truncated(void)
{
    uint8_t b[2]; cbor_w_t w; cbor_w_init(&w,b,sizeof b);
    cbor_text(&w,"beaucoup trop long pour deux octets");
    TEST_ASSERT(w.overflow, "le debordement est signale");
    TEST_ASSERT(w.len <= sizeof b, "rien n'est ecrit au-dela du tampon");
}

/* Le canari : un octet juste apres le tampon ne doit jamais changer. */
static void test_no_write_past_the_buffer(void)
{
    uint8_t b[9]; b[8] = 0xA5;
    cbor_w_t w; cbor_w_init(&w,b,8);
    cbor_bytes(&w,(const uint8_t*)"0123456789012345",16);
    TEST_ASSERT_EQ(b[8],0xA5,"le canari est intact");
}

/* ---- couverture complementaire : ce que les trois tests ci-dessus
 * n'atteignent jamais. ---- */

/* Les bornes 32 et 64 bits de la forme la plus courte : aucun des trois
 * tests precedents ne depasse 256, donc rien ne prouve que l'encodeur
 * bascule bien sur 5 puis 9 octets aux bonnes limites. Chaque paire est
 * comparee a sa voisine (longueur DIFFERENTE), pas a une constante isolee —
 * c'est le piege releve sur ce depot : un test qui croit couvrir une valeur
 * sans jamais la comparer a rien. */
static void test_uint_32_and_64_bit_boundaries(void)
{
    uint8_t b[16]; cbor_w_t w;

    cbor_w_init(&w,b,sizeof b); cbor_uint(&w,0xFFFFu);
    size_t len_ffff = w.len;
    TEST_ASSERT_EQ(len_ffff,3,"0xFFFF tient encore sur l'en-tete a 2 octets suivants");

    cbor_w_init(&w,b,sizeof b); cbor_uint(&w,0x10000u);
    TEST_ASSERT_EQ(w.len,5,"0x10000 bascule sur l'en-tete a 4 octets suivants");
    TEST_ASSERT_EQ(b[0],0x1A,"0x1A");
    TEST_ASSERT(w.len != len_ffff, "longueur differente de la voisine 0xFFFF");

    cbor_w_init(&w,b,sizeof b); cbor_uint(&w,0xFFFFFFFFu);
    size_t len_ffffffff = w.len;
    TEST_ASSERT_EQ(len_ffffffff,5,"0xFFFFFFFF tient encore sur 4 octets suivants");

    cbor_w_init(&w,b,sizeof b); cbor_uint(&w,0x100000000ull);
    TEST_ASSERT_EQ(w.len,9,"0x100000000 exige l'en-tete a 8 octets suivants");
    TEST_ASSERT_EQ(b[0],0x1B,"0x1B");
    TEST_ASSERT(w.len != len_ffffffff, "longueur differente de la voisine 0xFFFFFFFF");
}

/* cbor_array et cbor_map n'emettent qu'un en-tete, mais un en-tete de type
 * majeur different (4 contre 5) : rien dans les trois tests verbatim ne
 * l'exerce. Une mutation qui ferait emettre a cbor_map le meme type majeur
 * que cbor_array (4 au lieu de 5) romprait silencieusement tout decodeur en
 * aval — d'ou la comparaison deux a deux, pas juste "== 0xA2". */
static void test_array_and_map_headers_use_distinct_major_types(void)
{
    uint8_t b[4]; cbor_w_t w;

    cbor_w_init(&w,b,sizeof b); cbor_array(&w,2);
    TEST_ASSERT_EQ(w.len,1,"en-tete de tableau tient sur un octet pour n=2");
    uint8_t array_hdr = b[0];
    TEST_ASSERT_EQ(array_hdr,0x82,"type majeur 4 (0x80) | 2");

    cbor_w_init(&w,b,sizeof b); cbor_map(&w,2);
    TEST_ASSERT_EQ(w.len,1,"en-tete de map tient sur un octet pour n=2");
    uint8_t map_hdr = b[0];
    TEST_ASSERT_EQ(map_hdr,0xA2,"type majeur 5 (0xA0) | 2");

    TEST_ASSERT(array_hdr != map_hdr,
                "tableau et map n'utilisent jamais le meme octet d'en-tete pour n identique");
}

/* Meme logique pour bytes/text : deux types majeurs differents (2 et 3)
 * partageant le meme mecanisme de longueur. La comparaison porte sur les
 * deux en-tetes ET sur leurs deux tailles distinctes de sortie, pour croiser
 * chaine courte (forme immediate) et chaine plus longue (forme a un octet
 * suivant). */
static void test_bytes_and_text_headers_use_distinct_major_types(void)
{
    uint8_t b[8]; cbor_w_t w;

    cbor_w_init(&w,b,sizeof b); cbor_bytes(&w,(const uint8_t*)"abcde",5);
    TEST_ASSERT_EQ(w.len,6,"en-tete (1) + 5 octets de charge utile");
    uint8_t bytes_hdr = b[0];
    TEST_ASSERT_EQ(bytes_hdr,0x45,"type majeur 2 (0x40) | 5");

    cbor_w_init(&w,b,sizeof b); cbor_text(&w,"abcde");
    TEST_ASSERT_EQ(w.len,6,"meme taille totale que bytes pour un contenu de meme longueur");
    uint8_t text_hdr = b[0];
    TEST_ASSERT_EQ(text_hdr,0x65,"type majeur 3 (0x60) | 5");

    TEST_ASSERT(bytes_hdr != text_hdr,
                "chaine d'octets et chaine de texte n'utilisent jamais le meme en-tete");
}

/* Vrai et faux sont deux octets simples DIFFERENTS (0xF5/0xF4), pas juste
 * "un octet quelconque" : comparaison deux a deux. */
static void test_bool_encoding(void)
{
    uint8_t b[2]; cbor_w_t w;

    cbor_w_init(&w,b,sizeof b); cbor_bool(&w,true);
    TEST_ASSERT_EQ(w.len,1,"vrai tient sur un octet");
    uint8_t true_byte = b[0];
    TEST_ASSERT_EQ(true_byte,0xF5,"0xF5");

    cbor_w_init(&w,b,sizeof b); cbor_bool(&w,false);
    TEST_ASSERT_EQ(w.len,1,"faux tient sur un octet");
    uint8_t false_byte = b[0];
    TEST_ASSERT_EQ(false_byte,0xF4,"0xF4");

    TEST_ASSERT(true_byte != false_byte, "vrai et faux different toujours");
}

/* La borne de capacite elle-meme : un ecrit qui tient EXACTEMENT ne doit
 * jamais se signaler en debordement (sinon un `>=` remplacerait a tort le
 * `>` attendu sur la place restante), et un ecrit d'un octet de plus DOIT se
 * signaler. Deux verdicts distincts, compares l'un a l'autre — pas un seul
 * cas isole. */
static void test_exact_capacity_fit_is_not_flagged_overflow(void)
{
    uint8_t b[3]; cbor_w_t w;

    /* cbor_uint(300) => 0x19 + 2 octets = 3 octets, exactement la capacite. */
    cbor_w_init(&w,b,sizeof b);
    cbor_uint(&w,300);
    TEST_ASSERT(!w.overflow, "un encodage qui tient exactement n'est pas un debordement");
    TEST_ASSERT_EQ(w.len,3,"les 3 octets sont bien ecrits");

    /* Meme valeur, un octet de capacite en moins : doit deborder. */
    uint8_t b2[2]; cbor_w_t w2;
    cbor_w_init(&w2,b2,sizeof b2);
    cbor_uint(&w2,300);
    TEST_ASSERT(w2.overflow, "le meme encodage, un octet de capacite en moins, deborde");

    TEST_ASSERT(w.overflow != w2.overflow,
                "les deux verdicts different (comparaison deux a deux)");
}

void test_cbor_enc(void)
{
    TEST_SUITE("cbor_enc encodeur canonique");
    TEST_RUN(test_uint_uses_the_shortest_form);
    TEST_RUN(test_overflow_is_flagged_not_truncated);
    TEST_RUN(test_no_write_past_the_buffer);
    TEST_RUN(test_uint_32_and_64_bit_boundaries);
    TEST_RUN(test_array_and_map_headers_use_distinct_major_types);
    TEST_RUN(test_bytes_and_text_headers_use_distinct_major_types);
    TEST_RUN(test_bool_encoding);
    TEST_RUN(test_exact_capacity_fit_is_not_flagged_overflow);
}
