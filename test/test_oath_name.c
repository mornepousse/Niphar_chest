#include "test_framework.h"
#include "oath_name.h"
#include "sec_store.h"

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
        /* Ronde 3 : cible directement le poids de l'empreinte (Horner sur
         * l'issuer entier), pas seulement l'idee generale de collision. Meme
         * prefixe visible de 9 caracteres (« SERVICE12 »), meme dernier
         * octet (« X », poids base^0 = 1), et un AVANT-dernier octet qui
         * differe de PRECISEMENT 12 (« A »=0x41=65 contre « M »=0x4D=77) — le
         * seul octet dont le poids est base^1. Avec une base 33,
         * 33*12 = 396 = 11*36 ≡ 0 (mod 36) : l'empreinte entiere ne bouge
         * pas, collision garantie quel que soit le reste du nom. Avec 31,
         * 31*12 = 372 ≡ 12 (mod 36) : non nul, empreintes distinctes. Ne pas
         * changer ces caracteres sans refaire le calcul — n'importe quelle
         * autre paire distante de 12 a cette position rejoue la meme preuve,
         * mais une distance differente (ou une position differente) ne la
         * rejoue PAS forcement. */
        { "SERVICE12AX", "SERVICE12MX" },
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


/* ------------------------------------------------------------------------- */
/* L'etiquette d'un RESET : combien de comptes partent.                       */
/* ------------------------------------------------------------------------- */

/*
 * Cette chaine n'est PAS un nom venu de l'hote — c'est nous qui la
 * fabriquons — mais elle traverse exactement le meme traitement
 * (sec_confirm_arm_named -> oath_name_display) : coupe au premier ':',
 * retrait d'un prefixe numerique suivi de '/', assainissement, troncature a
 * dix caracteres. Une forme mal choisie ressortirait tronquee ou vide, et
 * l'ecran annoncerait un effacement total sans dire combien de comptes
 * partent — l'inverse exact de ce que cette etiquette existe pour dire.
 */
static void test_reset_label_dit_le_nombre(void)
{
    char out[OATH_RESET_LABEL_MAX];

    oath_reset_label(12, out, sizeof(out));
    TEST_ASSERT(strcmp(out, "12 COMPTES") == 0, "douze comptes se disent en toutes lettres");

    oath_reset_label(16, out, sizeof(out));
    TEST_ASSERT(strcmp(out, "16 COMPTES") == 0, "le magasin plein aussi");
}

/* Un compte au singulier, zero aussi (« 0 compte » en francais). Verifie en
 * COMPARANT les trois formes entre elles : un pluriel colle partout rendrait
 * « 1 COMPTES », qu'une assertion « la chaine contient le chiffre » ne
 * verrait pas. */
static void test_reset_label_accorde_le_singulier(void)
{
    char un[OATH_RESET_LABEL_MAX], zero[OATH_RESET_LABEL_MAX], deux[OATH_RESET_LABEL_MAX];

    oath_reset_label(1, un, sizeof(un));
    oath_reset_label(0, zero, sizeof(zero));
    oath_reset_label(2, deux, sizeof(deux));

    TEST_ASSERT(strcmp(un, "1 COMPTE") == 0, "un seul compte, au singulier");
    TEST_ASSERT(strcmp(zero, "0 COMPTE") == 0, "zero compte, au singulier aussi");
    TEST_ASSERT(strcmp(deux, "2 COMPTES") == 0, "deux comptes, au pluriel");
    TEST_ASSERT(strcmp(un, deux) != 0, "singulier et pluriel ne se confondent pas");
}

/*
 * LE test de ce bloc : la chaine doit traverser oath_name_display() SANS
 * PERDRE UN OCTET, pour toutes les valeurs que le magasin peut produire.
 *
 * L'egalite stricte, et pas « non vide » : c'est elle qui attrape une forme
 * qui contiendrait un ':' (coupee), un prefixe « 30/ » (ampute), ou qui
 * depasserait les dix caracteres visibles (tronquee, avec une empreinte a la
 * place du dernier chiffre). Aucun de ces trois defauts ne se voit sur la
 * seule chaine formatee — ils n'apparaissent qu'apres le traitement.
 */
static void test_reset_label_traverse_l_affichage_intact(void)
{
    for (unsigned n = 0; n <= SEC_N_SLOTS; n++) {
        char brut[OATH_RESET_LABEL_MAX];
        char vu[OATH_NAME_DISPLAY_MAX];

        oath_reset_label((uint8_t)n, brut, sizeof(brut));
        TEST_ASSERT(brut[0] != '\0', "l'etiquette n'est jamais vide");

        oath_name_display(brut, (uint16_t)strlen(brut), vu, sizeof(vu));
        TEST_ASSERT(strcmp(vu, brut) == 0,
                    "l'etiquette ressort de l'affichage telle qu'elle y est entree");
    }
}

/* Tampon trop court : chaine vide plutot qu'un nombre ampute — « 1 » a la
 * place de « 16 » ferait autoriser l'effacement de seize comptes en croyant
 * en effacer un. Et rien n'est ecrit au-dela de out_sz. */
static void test_reset_label_borne_le_tampon(void)
{
    for (uint8_t sz = 1; sz < OATH_RESET_LABEL_MAX; sz++) {
        char tampon[OATH_RESET_LABEL_MAX + 4];
        memset(tampon, 0xAA, sizeof(tampon));
        oath_reset_label(16, tampon, sz);

        for (unsigned k = sz; k < sizeof(tampon); k++) {
            TEST_ASSERT((unsigned char)tampon[k] == 0xAAu,
                        "rien n'est ecrit au-dela de out_sz");
        }
        /* « 16 COMPTES » fait dix caracteres : en dessous de onze octets, la
         * seule sortie honnete est la chaine vide. */
        if (sz < 11) {
            TEST_ASSERT(tampon[0] == '\0', "trop court : chaine vide, jamais un nombre ampute");
        } else {
            TEST_ASSERT(strcmp(tampon, "16 COMPTES") == 0, "onze octets suffisent");
        }
    }

    /* out_sz == 0 ne doit rien ecrire du tout. */
    char rien[2] = { 0x5A, 0x5A };
    oath_reset_label(3, rien, 0);
    TEST_ASSERT((unsigned char)rien[0] == 0x5Au, "out_sz nul : pas un octet ecrit");
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
    TEST_RUN(test_reset_label_dit_le_nombre);
    TEST_RUN(test_reset_label_accorde_le_singulier);
    TEST_RUN(test_reset_label_traverse_l_affichage_intact);
    TEST_RUN(test_reset_label_borne_le_tampon);
}
