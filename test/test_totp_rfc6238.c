/*
 * La chaine TOTP du coffre, bout en bout, contre les vecteurs OFFICIELS de la
 * RFC 6238 (annexe B).
 *
 * POURQUOI CE FICHIER EXISTE. Le code TOTP que le clavier affichera traverse
 * quatre modules : sec_time (le numero de fenetre), cr_hmac (le HMAC),
 * oath_proto (la troncature dynamique) et link_proto (le modulo et le
 * remplissage). Chacun etait teste separement — et c'est precisement la forme
 * d'erreur qui survit a des tests unitaires tous verts : quatre maillons justes
 * peuvent s'enchainer de travers.
 *
 * L'ORACLE EST EXTERIEUR, et c'est tout l'interet. Les codes attendus ne
 * viennent pas de moi : ils sont PUBLIES dans la RFC 6238, et ils ont ete
 * reproduits independamment par oathtool 2.6.14 et par la bibliotheque standard
 * de Python avant d'etre ecrits ici. Un test dont j'ecris moi-meme la reponse
 * attendue ne prouve que ma coherence avec moi-meme.
 *
 * CE QUI N'EST PAS COUVERT, ET IL FAUT LE DIRE : cr_hmac_sha1() lui-meme. Il
 * passe par mbedtls, qui n'existe pas sur l'hote, donc les HMAC ci-dessous sont
 * EPINGLES depuis l'oracle au lieu d'etre calcules par le coffre. C'est le
 * maillon le plus mince des quatre — une delegation de dix lignes a une
 * bibliotheque eprouvee — mais il reste a prouver sur materiel.
 */
#include "test_framework.h"

#include "link/link_proto.h"
#include "oath_proto.h"
#include "sec_time.h"

#include <string.h>

/* Secret de la RFC 6238 pour SHA-1 : l'ASCII « 12345678901234567890 ». */
struct vecteur {
    uint64_t    unix_s;
    uint64_t    compteur;
    uint8_t     hmac[20];
    const char *code8;
};

static const struct vecteur k_rfc6238[] = {
    {            59ull, 0x00000001ull, {0x75,0xa4,0x8a,0x19,0xd4,0xcb,0xe1,0x00,0x64,0x4e,0x8a,0xc1,0x39,0x7e,0xea,0x74,0x7a,0x2d,0x33,0xab}, "94287082" },
    {    1111111109ull, 0x023523ECull, {0x27,0x8c,0x02,0xe5,0x36,0x10,0xf8,0x4c,0x40,0xbd,0x91,0x35,0xac,0xd4,0x10,0x10,0x12,0x41,0x0a,0x14}, "07081804" },
    {    1111111111ull, 0x023523EDull, {0xb0,0x09,0x2b,0x21,0xd0,0x48,0xaf,0x20,0x9d,0xa0,0xa1,0xdd,0xd4,0x98,0xad,0xe8,0xa7,0x94,0x87,0xed}, "14050471" },
    {    1234567890ull, 0x0273EF07ull, {0x90,0x7c,0xd1,0xa9,0x11,0x65,0x64,0xec,0xb9,0xd5,0xd1,0x78,0x03,0x25,0xf2,0x46,0x17,0x3f,0xe7,0x03}, "89005924" },
    {    2000000000ull, 0x03F940AAull, {0x25,0xa3,0x26,0xd3,0x1f,0xc3,0x66,0x24,0x4c,0xad,0x05,0x49,0x76,0x02,0x0c,0x7b,0x56,0xb1,0x3d,0x5f}, "69279037" },
    {   20000000000ull, 0x27BC86AAull, {0xab,0x07,0xe9,0x7e,0x2c,0x12,0x78,0x76,0x9d,0xbc,0xd7,0x57,0x83,0xaa,0xbd,0xe7,0x5e,0xd8,0x55,0x0a}, "65353130" },
};

#define N_RFC (sizeof(k_rfc6238) / sizeof(k_rfc6238[0]))

/* Le numero de fenetre, seul maillon que sec_time fournit. Le dernier vecteur
 * depasse 2^32 : une variable de 32 bits le ferait reboucler en silence, et le
 * coffre calculerait le code d'une autre annee. */
static void test_le_compteur_suit_la_rfc(void)
{
    for (unsigned i = 0; i < N_RFC; i++) {
        TEST_ASSERT_EQ(sec_time_totp_counter(k_rfc6238[i].unix_s),
                       k_rfc6238[i].compteur, "numero de fenetre RFC 6238");
    }
    TEST_ASSERT(k_rfc6238[N_RFC - 1].unix_s > 0xFFFFFFFFull,
                "le dernier vecteur passe au-dela de 32 bits, et c'est voulu");
}

/* LA CHAINE ENTIERE : HMAC -> troncature dynamique -> modulo -> remplissage.
 * C'est ce test qui vaut, parce qu'il compare a une reference que je n'ai pas
 * ecrite. */
static void test_la_chaine_rend_les_codes_de_la_rfc(void)
{
    char out[9];
    for (unsigned i = 0; i < N_RFC; i++) {
        const uint32_t dbc = oath_dynamic_binary(k_rfc6238[i].hmac, 20);
        TEST_ASSERT_EQ(link_proto_format_code(dbc, 8, out), 8, "huit chiffres");
        TEST_ASSERT_EQ(memcmp(out, k_rfc6238[i].code8, 9), 0,
                       "code RFC 6238 a huit chiffres");
    }
}

/*
 * Six chiffres : ce sont exactement les SIX DERNIERS du code a huit, parce que
 * 10^6 divise 10^8. La propriete n'est pas anecdotique — elle dit que le
 * nombre de chiffres ne change QUE la longueur, jamais la valeur. Un modulo
 * applique au mauvais endroit casserait ca sans casser le cas a huit chiffres.
 */
static void test_six_chiffres_sont_les_six_derniers(void)
{
    char huit[9], six[9];
    for (unsigned i = 0; i < N_RFC; i++) {
        const uint32_t dbc = oath_dynamic_binary(k_rfc6238[i].hmac, 20);
        TEST_ASSERT_EQ(link_proto_format_code(dbc, 8, huit), 8, "huit");
        TEST_ASSERT_EQ(link_proto_format_code(dbc, 6, six), 6, "six");
        TEST_ASSERT_EQ(memcmp(six, huit + 2, 7), 0,
                       "les six chiffres sont la queue des huit");
    }
}

/*
 * Le decompte affiche a cote du code. Il n'a pas de vecteur RFC, mais il a une
 * propriete qui se verifie sur les memes instants : il vaut 30 au debut d'une
 * fenetre et 1 a sa derniere seconde, jamais 0 — un decompte a zero afficherait
 * un code deja perime comme s'il restait a l'utiliser.
 */
static void test_le_decompte_accompagne_la_fenetre(void)
{
    for (unsigned i = 0; i < N_RFC; i++) {
        const uint64_t t = k_rfc6238[i].unix_s;
        const uint64_t debut = k_rfc6238[i].compteur * SEC_TIME_TOTP_STEP;
        TEST_ASSERT_EQ(sec_time_window_remaining(debut), SEC_TIME_TOTP_STEP,
                       "debut de fenetre : tout le temps");
        TEST_ASSERT_EQ(sec_time_window_remaining(debut + SEC_TIME_TOTP_STEP - 1u), 1,
                       "derniere seconde : un");
        const uint8_t r = sec_time_window_remaining(t);
        TEST_ASSERT(r >= 1u && r <= SEC_TIME_TOTP_STEP, "toujours dans 1..30");
        /* Et le code ne change pas dans la fenetre : meme compteur du debut a
         * la fin, c'est ce qui rend le decompte utile. */
        TEST_ASSERT_EQ(sec_time_totp_counter(debut),
                       sec_time_totp_counter(debut + SEC_TIME_TOTP_STEP - 1u),
                       "meme fenetre, meme code");
        TEST_ASSERT(sec_time_totp_counter(debut + SEC_TIME_TOTP_STEP)
                    != sec_time_totp_counter(debut),
                    "la seconde d'apres change de fenetre");
    }
}

/*
 * L'heure POSEE, avancee sur le temps monotone, retombe sur le bon code. C'est
 * le chemin reel : l'hote pose l'heure une fois, et le coffre calcule des
 * secondes ou des minutes plus tard sur son propre compteur.
 */
static void test_une_heure_posee_puis_avancee_rend_le_bon_code(void)
{
    /* Le vecteur de 2033, et PAS un des historiques : sec_time_set() refuse
     * tout ce qui precede 2024 (SEC_TIME_MIN_PLAUSIBLE). Les quatre premiers
     * vecteurs de la RFC datent de 1970 a 2009 — ils servent a verifier le
     * CALCUL, pas a etre poses comme heure murale. Le test l'a appris en
     * echouant sur « heure posee », ce qui est le garde-fou qui fait son
     * travail. */
    const struct vecteur *v = &k_rfc6238[4];   /* 2000000000, soit 2033 */
    char out[9];

    sec_time_reset();
    /* Posee CINQ SECONDES plus tot, puis avancee de cinq mille millisecondes :
     * le coffre doit retrouver l'instant du vecteur. */
    TEST_ASSERT(sec_time_set(v->unix_s - 5u, 1000u), "heure posee");

    uint64_t maintenant = 0;
    TEST_ASSERT(sec_time_now(6000u, &maintenant), "heure lisible");
    TEST_ASSERT_EQ(maintenant, v->unix_s, "cinq secondes de temps monotone plus tard");
    TEST_ASSERT_EQ(sec_time_totp_counter(maintenant), v->compteur, "bonne fenetre");

    const uint32_t dbc = oath_dynamic_binary(v->hmac, 20);
    TEST_ASSERT_EQ(link_proto_format_code(dbc, 8, out), 8, "huit chiffres");
    TEST_ASSERT_EQ(memcmp(out, v->code8, 9), 0,
                   "le code de la RFC, obtenu par le chemin reel");
    sec_time_reset();
}

/*
 * LE PLANCHER DE PLAUSIBILITE REFUSE LES VECTEURS HISTORIQUES, ET C'EST JUSTE.
 *
 * Les quatre premiers vecteurs de la RFC datent de 1970, 2005 et 2009. Ils
 * verifient le CALCUL, pas la pose d'une heure murale — et un coffre a qui on
 * annonce 2009 tient une heure qui n'en est pas une. Ce test fige la frontiere
 * plutot que de la laisser se decouvrir en echec, comme ca vient d'arriver.
 */
static void test_le_plancher_refuse_les_vecteurs_historiques(void)
{
    sec_time_reset();
    for (unsigned i = 0; i < N_RFC; i++) {
        const bool pose = sec_time_set(k_rfc6238[i].unix_s, 1000u);
        const bool plausible = k_rfc6238[i].unix_s >= SEC_TIME_MIN_PLAUSIBLE;
        TEST_ASSERT_EQ(pose, plausible,
                       "posee si et seulement si au-dessus du plancher");
        sec_time_reset();
    }
    /* Et la frontiere est bien traversee par ce jeu de vecteurs : sans ca, le
     * test ci-dessus serait vrai par accident. */
    TEST_ASSERT(k_rfc6238[0].unix_s < SEC_TIME_MIN_PLAUSIBLE, "au moins un en dessous");
    TEST_ASSERT(k_rfc6238[N_RFC - 1].unix_s >= SEC_TIME_MIN_PLAUSIBLE, "au moins un au-dessus");
}

void test_totp_rfc6238(void)
{
    TEST_SUITE("TOTP — vecteurs RFC 6238");
    TEST_RUN(test_le_compteur_suit_la_rfc);
    TEST_RUN(test_la_chaine_rend_les_codes_de_la_rfc);
    TEST_RUN(test_six_chiffres_sont_les_six_derniers);
    TEST_RUN(test_le_decompte_accompagne_la_fenetre);
    TEST_RUN(test_le_plancher_refuse_les_vecteurs_historiques);
    TEST_RUN(test_une_heure_posee_puis_avancee_rend_le_bon_code);
}
