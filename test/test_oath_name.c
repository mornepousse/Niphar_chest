#include "test_framework.h"
#include "oath_name.h"

/* Le nom YKOATH s'ecrit « [periode/][Issuer:]compte ». Ce que Mae reconnait
 * d'un coup d'oeil est l'issuer, pas l'adresse. */
static void test_issuer_extrait(void)
{
    char out[OATH_NAME_DISPLAY_MAX];
    const char *n = "GitHub:mae@exemple.org";
    oath_name_display(n, (uint16_t)strlen(n), out, sizeof(out));
    TEST_ASSERT(strcmp(out, "GITHUB") == 0, "l'issuer seul, en majuscules");

    const char *p = "30/GitLab:mae";
    oath_name_display(p, (uint16_t)strlen(p), out, sizeof(out));
    TEST_ASSERT(strcmp(out, "GITLAB") == 0, "le prefixe de periode est retire");
}

/*
 * LE test de ce fichier. Deux noms DIFFERENTS doivent rester DISTINGUABLES a
 * l'ecran : c'est toute la raison d'afficher le nom. Verifier separement que
 * « a » rend « A » ne prouve rien la-dessus — un assainissement qui rendrait
 * la meme chose pour tout passerait.
 */
static void test_noms_distincts_restent_distincts(void)
{
    const char *paires[][2] = {
        { "GitHub:mae",        "GitHub mae"  },   /* ponctuation vs espace */
        { "Banque",            "Ban\x01que"  },   /* caractere de controle */
        { "MonServiceTresLong1", "MonServiceTresLong2" }, /* divergence tardive */
        /* Meme prefixe visible APRES assainissement (deux octets de controle
         * distincts retombent tous deux sur « ? »), meme queue masquee : si
         * l'empreinte ne portait que sur la queue, ces deux noms rendraient
         * la meme chaine — c'est exactement le trou que corrige la ronde 1. */
        { "Ban\x01que" "XXXXXXXXXXXX", "Ban\x02que" "XXXXXXXXXXXX" },
    };
    for (unsigned i = 0; i < sizeof(paires) / sizeof(paires[0]); i++) {
        char a[OATH_NAME_DISPLAY_MAX], b[OATH_NAME_DISPLAY_MAX];
        oath_name_display(paires[i][0], (uint16_t)strlen(paires[i][0]), a, sizeof(a));
        oath_name_display(paires[i][1], (uint16_t)strlen(paires[i][1]), b, sizeof(b));
        TEST_ASSERT(strcmp(a, b) != 0,
                    "deux noms differents ne doivent pas s'afficher pareil");
    }
}

/* Un caractere sans glyphe devient « ? », jamais un blanc : un nom bricole doit
 * se VOIR, pas se deguiser en nom propre. */
static void test_caractere_indessinable_visible(void)
{
    char out[OATH_NAME_DISPLAY_MAX];
    const char *n = "Ban\x01que";
    oath_name_display(n, (uint16_t)strlen(n), out, sizeof(out));
    TEST_ASSERT(strchr(out, '?') != NULL, "le caractere de controle laisse une trace");
    TEST_ASSERT(strchr(out, '\x01') == NULL, "l'octet brut ne passe pas");
}

/* Troncature marquee : sans marqueur, deux noms qui divergent au-dela du
 * dixieme caractere seraient indiscernables et l'appui redeviendrait aveugle. */
static void test_troncature_marquee(void)
{
    char out[OATH_NAME_DISPLAY_MAX];
    const char *n = "ServiceExtremementLong";
    oath_name_display(n, (uint16_t)strlen(n), out, sizeof(out));
    TEST_ASSERT(strlen(out) == 11, "dix caracteres plus le marqueur");
    TEST_ASSERT(out[10] == '?', "le marqueur de troncature termine la ligne");

    const char *court = "Court";
    oath_name_display(court, (uint16_t)strlen(court), out, sizeof(out));
    TEST_ASSERT(out[strlen(out) - 1] != '?', "un nom court ne porte pas de marqueur");
}

/* Entrees degenerees : la sortie est TOUJOURS une chaine valide. Une fonction
 * d'affichage qui laisse `out` non initialise fait dessiner de la pile. */
static void test_entrees_degenerees(void)
{
    char out[OATH_NAME_DISPLAY_MAX];
    oath_name_display("", 0, out, sizeof(out));
    TEST_ASSERT(out[0] == '\0', "nom vide -> chaine vide, pas de pile dessinee");
    oath_name_display(NULL, 0, out, sizeof(out));
    TEST_ASSERT(out[0] == '\0', "nom absent -> chaine vide");
    oath_name_display(":", 1, out, sizeof(out));
    TEST_ASSERT(out[0] == '\0', "issuer vide -> chaine vide");
}

/* Bords de out_sz : la fonction promet toujours une chaine terminee, y
 * compris pour un tampon trop petit pour montrer quoi que ce soit d'utile.
 * Avec un nom assez long pour tronquer, out_sz=1 et out_sz=2 ne laissent pas
 * la place d'un caractere, du marqueur ET du terminateur — elle doit alors se
 * limiter a la chaine vide, jamais deborder au-dela de out_sz. Un debordement
 * qui ne se voit pas est un debordement qu'on croit absent : on verifie donc
 * qu'aucun octet au-dela de out_sz n'est touche, pas seulement le contenu. */
static void test_out_sz_borne(void)
{
    const char *n = "ServiceExtremementLong";   /* assez long pour tronquer */
    for (uint8_t sz = 1; sz <= 3; sz++) {
        char tampon[8];
        memset(tampon, 0xAA, sizeof(tampon));
        oath_name_display(n, (uint16_t)strlen(n), tampon, sz);

        for (uint8_t k = sz; k < sizeof(tampon); k++) {
            TEST_ASSERT((unsigned char)tampon[k] == 0xAA,
                        "rien n'est ecrit au-dela de out_sz");
        }
        bool termine = false;
        for (uint8_t k = 0; k < sz; k++) {
            if (tampon[k] == '\0') { termine = true; break; }
        }
        TEST_ASSERT(termine, "la chaine se termine dans les bornes de out_sz");
    }
}

void test_oath_name(void)
{
    TEST_SUITE("oath_name");
    TEST_RUN(test_issuer_extrait);
    TEST_RUN(test_noms_distincts_restent_distincts);
    TEST_RUN(test_caractere_indessinable_visible);
    TEST_RUN(test_troncature_marquee);
    TEST_RUN(test_entrees_degenerees);
    TEST_RUN(test_out_sz_borne);
}
