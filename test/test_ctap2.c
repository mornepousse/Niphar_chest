#include "test_framework.h"
#include "ctap2.h"
#include "ctaphid.h"
#include <string.h>

/*
 * AAGUID attendu, copie INDEPENDANTE de celle codee dans ctap2.c : le but
 * de ce test est justement de faire echouer la suite si quelqu'un change
 * cette valeur committee sans s'en rendre compte. Un AAGUID identifie un
 * MODELE d'authentificateur, pas un exemplaire — il ne doit jamais varier
 * d'une carte a l'autre (voir le commentaire dans ctap2.c).
 */
static const uint8_t k_expected_aaguid[16] = {
    0x76, 0x36, 0x55, 0x35, 0xe5, 0x58, 0x4f, 0x54,
    0xb3, 0x2d, 0x5f, 0xc7, 0x94, 0x26, 0xa6, 0x28,
};

/* Le statut CTAP2 est le tout premier octet de la reponse, avant meme le
 * CBOR : c'est ce que lit tout client CTAP2 pour savoir si la commande a
 * reussi. */
static void test_status_byte_is_ctap2_ok(void)
{
    uint8_t out[128];
    size_t n = ctap2_build_get_info(out, sizeof out);
    TEST_ASSERT(n > 0, "un tampon largement suffisant ne doit jamais deborder");
    TEST_ASSERT_EQ(out[0], CTAP2_OK, "premier octet : CTAP2_OK (0x00)");
}

/*
 * Recommandation du relecteur de la tache 4 (2026-08-18) : cbor_enc.c ne
 * voit jamais la map complete, seulement des appels successifs — il ne peut
 * donc PAS garantir l'ordre canonique des cles. Cette garantie est
 * entierement a la charge de l'appelant (ctap2.c), et rien dans
 * l'interface de cbor_enc.h n'avertit d'une erreur d'ordre : le CBOR
 * produit resterait syntaxiquement valide, juste refuse par un client qui
 * verifie le canonique (RFC 8949 §4.2.1, exige par CTAP2). Ce test lit donc
 * les OCTETS PRODUITS et verifie que les cles y apparaissent en ordre
 * strictement croissant — le seul endroit ou cette propriete est
 * verifiable. Un commentaire affirmant "c'est deja croissant" ne mord pas ;
 * ceci mord.
 *
 * Format REVISE (ruling du coordinateur, 2026-08-18, revue Critique de cette
 * meme tache) : "FIDO_2_0" et "rk" sont retires — la reponse ne doit annoncer
 * que ce que la cle saura reellement faire A LA FIN DU PLAN. Voir ctap2.c
 * pour la justification complete.
 *
 * Format de la reponse (bytes dument comptes a la main) :
 *   [0]      statut CTAP2_OK
 *   [1]      0xA4              map(4)                — toujours 4 cles au sommet
 *   [2]      0x01              cle 1  (versions)
 *   [3]      0x81              array(1)               — SEUL "U2F_V2", plus "FIDO_2_0"
 *   [4..10]  "U2F_V2"          text(6), entete inclus
 *   [11]     0x03              cle 3  (aaguid)
 *   [12..28] bytes(16)         entete + aaguid
 *   [29]     0x04              cle 4  (options)
 *   [30]     0xA1              map(1)                 — SEUL "up", plus "rk"
 *   [31..33] "up"              text(2), entete inclus
 *   [34]     0xF5              bool vrai
 *   [35]     0x05              cle 5  (maxMsgSize)
 *   [36..38] uint(7609)        entete 2 octets + valeur
 * Longueur totale : 39 octets — bien sous les 57 d'un unique paquet
 * d'initialisation CTAP-HID (la fragmentation n'est pas exercee par cette
 * reponse precise, voir le rapport de tache 5).
 */
static void test_get_info_key_order_is_canonical(void)
{
    uint8_t out[128];
    size_t n = ctap2_build_get_info(out, sizeof out);
    TEST_ASSERT_EQ(n, 39, "longueur totale exacte de la reponse (voir calcul en commentaire)");

    TEST_ASSERT_EQ(out[1], 0xA4, "map de 4 paires au sommet (major type 5 | 4) — inchange par le retrait");

    /* Les quatre cles ENTIERES de la map principale, dans l'ORDRE DES
     * OCTETS PRODUITS : 1 < 3 < 4 < 5. Comparees deux a deux, pas a des
     * constantes isolees — c'est ce qui romprait si un futur appel
     * reordonnait par exemple options avant aaguid. */
    uint8_t key_versions   = out[2];
    uint8_t key_aaguid     = out[11];
    uint8_t key_options    = out[29];
    uint8_t key_maxmsgsize = out[35];
    TEST_ASSERT_EQ(key_versions, 1, "premiere cle produite : 1 (versions)");
    TEST_ASSERT_EQ(key_aaguid, 3, "deuxieme cle produite : 3 (aaguid)");
    TEST_ASSERT_EQ(key_options, 4, "troisieme cle produite : 4 (options)");
    TEST_ASSERT_EQ(key_maxmsgsize, 5, "quatrieme cle produite : 5 (maxMsgSize)");
    TEST_ASSERT(key_versions < key_aaguid, "1 < 3 dans l'ordre d'ecriture reel");
    TEST_ASSERT(key_aaguid < key_options, "3 < 4 dans l'ordre d'ecriture reel");
    TEST_ASSERT(key_options < key_maxmsgsize, "4 < 5 dans l'ordre d'ecriture reel");

    /* versions : UN SEUL element, "U2F_V2". */
    TEST_ASSERT_EQ(out[3], 0x81, "array(1) : un seul element dans versions");
    TEST_ASSERT(memcmp(out + 4, "\x66U2F_V2", 7) == 0, "\"U2F_V2\" juste apres l'array");

    /* options : UNE SEULE cle, "up". */
    TEST_ASSERT_EQ(out[30], 0xA1, "sous-map d'UNE SEULE paire (options) — \"rk\" retire");
    TEST_ASSERT(memcmp(out + 31, "\x62up", 3) == 0, "cle texte \"up\", seule presente dans options");
    TEST_ASSERT_EQ(out[34], 0xF5, "\"up\" vaut vrai");
}

/*
 * "FIDO_2_0" et "rk" doivent etre ABSENTS : c'est le cœur du Critique de la
 * revue (2026-08-18) — la reponse annoncait un support CTAP2 complet
 * (creances residentes comprises) qu'aucune commande de creance ne cable
 * dans ce plan (makeCredential/getAssertion, decodeur CBOR general : tous
 * reportes au plan 2). clientPin et uv restent absents pour la meme raison
 * qu'avant (spec sans PIN). Les quatre verifications portent sur les octets
 * REELLEMENT produits, pas sur une relecture du code source. */
static void test_unimplemented_capabilities_are_absent(void)
{
    uint8_t out[128];
    size_t n = ctap2_build_get_info(out, sizeof out);

    /* "FIDO_2_0" encode en CBOR commencerait par 0x68 (text, longueur 8). */
    int found_fido2 = 0;
    for (size_t i = 0; i + 9 <= n; i++) {
        if (out[i] == 0x68 && memcmp(out + i + 1, "FIDO_2_0", 8) == 0) {
            found_fido2 = 1;
        }
    }
    TEST_ASSERT(!found_fido2, "\"FIDO_2_0\" n'apparait nulle part : aucune commande de creance cablee");

    /* "rk" encode commencerait par 0x62 (text, longueur 2) suivi de "rk" —
     * a distinguer de "up", present et legitime ailleurs dans la reponse. */
    int found_rk = 0;
    for (size_t i = 0; i + 3 <= n; i++) {
        if (out[i] == 0x62 && out[i + 1] == 'r' && out[i + 2] == 'k') {
            found_rk = 1;
        }
    }
    TEST_ASSERT(!found_rk, "\"rk\" n'apparait nulle part : aucun magasin de creances residentes");

    /* "clientPin" encode en CBOR commencerait par 0x69 (text, longueur 9). */
    int found_client_pin = 0;
    for (size_t i = 0; i + 10 <= n; i++) {
        if (out[i] == 0x69 && memcmp(out + i + 1, "clientPin", 9) == 0) {
            found_client_pin = 1;
        }
    }
    TEST_ASSERT(!found_client_pin, "\"clientPin\" n'apparait nulle part dans la reponse");

    /* "uv" encode commencerait par 0x62 (text, longueur 2) suivi de "uv". */
    int found_uv = 0;
    for (size_t i = 0; i + 3 <= n; i++) {
        if (out[i] == 0x62 && out[i + 1] == 'u' && out[i + 2] == 'v') {
            found_uv = 1;
        }
    }
    TEST_ASSERT(!found_uv, "\"uv\" n'apparait nulle part dans la reponse");
}

/* L'AAGUID est un identifiant de MODELE, fige en dur : ce test le compare a
 * une copie independante (k_expected_aaguid ci-dessus) pour detecter tout
 * changement accidentel — y compris un octet qui varierait par exemplaire,
 * ce que l'AAGUID ne doit JAMAIS faire. */
static void test_aaguid_matches_committed_value(void)
{
    uint8_t out[128];
    size_t n = ctap2_build_get_info(out, sizeof out);
    TEST_ASSERT(n >= 29, "assez d'octets pour contenir l'aaguid");
    TEST_ASSERT_EQ(out[12], 0x50, "en-tete bytes(16) pour l'aaguid");
    TEST_ASSERT(memcmp(out + 13, k_expected_aaguid, 16) == 0,
                "les 16 octets de l'aaguid correspondent a la valeur committee");
}

/* maxMsgSize doit correspondre exactement a CTAPHID_MAX_PAYLOAD : annoncer
 * une valeur differente tromperait un client sur ce qu'il peut envoyer en
 * un seul message CTAP-HID. */
static void test_max_msg_size_matches_ctaphid_max_payload(void)
{
    uint8_t out[128];
    size_t n = ctap2_build_get_info(out, sizeof out);
    TEST_ASSERT(n >= 39, "assez d'octets pour contenir maxMsgSize");
    TEST_ASSERT_EQ(out[36], 0x19, "entete uint 2 octets suivants (7609 > 255)");
    uint16_t value = (uint16_t)((out[37] << 8) | out[38]);
    TEST_ASSERT_EQ(value, CTAPHID_MAX_PAYLOAD, "maxMsgSize == CTAPHID_MAX_PAYLOAD");
}

/* Un tampon trop petit doit se signaler par un retour 0 — jamais une
 * ecriture partielle qu'un appelant distrait prendrait pour une reponse
 * complete. Meme discipline que cbor_enc.h (overflow verrouille). */
static void test_buffer_too_small_returns_zero(void)
{
    uint8_t out[10];
    size_t n = ctap2_build_get_info(out, sizeof out);
    TEST_ASSERT_EQ(n, 0, "10 octets ne suffisent pas pour la reponse complete (39 attendus)");
}

/* Deux constructions successives doivent produire des octets identiques :
 * aucun etat cache, aucune dependance a un generateur aleatoire — la
 * reponse est entierement determinee par des constantes fixes. */
static void test_response_is_deterministic(void)
{
    uint8_t a[128], b[128];
    size_t na = ctap2_build_get_info(a, sizeof a);
    size_t nb = ctap2_build_get_info(b, sizeof b);
    TEST_ASSERT_EQ(na, nb, "meme longueur d'un appel a l'autre");
    TEST_ASSERT(memcmp(a, b, na) == 0, "memes octets d'un appel a l'autre");
}

void test_ctap2(void)
{
    TEST_SUITE("ctap2 authenticatorGetInfo");
    TEST_RUN(test_status_byte_is_ctap2_ok);
    TEST_RUN(test_get_info_key_order_is_canonical);
    TEST_RUN(test_unimplemented_capabilities_are_absent);
    TEST_RUN(test_aaguid_matches_committed_value);
    TEST_RUN(test_max_msg_size_matches_ctaphid_max_payload);
    TEST_RUN(test_buffer_too_small_returns_zero);
    TEST_RUN(test_response_is_deterministic);
}
