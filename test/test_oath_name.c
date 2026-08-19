#include "test_framework.h"
#include "oath_name.h"
#include "sec_store.h"

/*
 * Le nom YKOATH s'ecrit « [periode/][Issuer:]compte ». Le nom COMPLET est
 * affiche — emetteur ET compte — parce que la proprietaire a deux comptes
 * chez le meme emetteur (voir test_noms_distincts_restent_distincts). Seul le
 * prefixe de periode part : c'est du protocole, pas du sens.
 */
static void test_nom_complet_affiche(void)
{
    char out[OATH_NAME_DISPLAY_MAX];
    const char *n = "GitHub:mae@ex.org";
    oath_name_display(n, (uint16_t)strlen(n), out, sizeof(out));
    TEST_ASSERT(strcmp(out, "GITHUB:MAE@EX.ORG") == 0,
                "le nom entier, en majuscules, ':' compris");

    const char *p = "30/GitLab:mae";
    oath_name_display(p, (uint16_t)strlen(p), out, sizeof(out));
    TEST_ASSERT(strcmp(out, "GITLAB:MAE") == 0,
                "le prefixe de periode est retire, le reste est garde");

    /* Le compte SEUL ne suffit pas non plus : c'est la paire qui identifie. */
    const char *q = "OVH:pro";
    oath_name_display(q, (uint16_t)strlen(q), out, sizeof(out));
    TEST_ASSERT(strcmp(out, "OVH:PRO") == 0, "emetteur et compte, tous deux");
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
        /*
         * DEUX COMPTES CHEZ LE MEME EMETTEUR. La proprietaire en a : OVH et
         * Ankama auront chacun un compte perso et un compte pro. La regle
         * « ne garder que l'emetteur » les rendait strictement indiscernables
         * a l'ecran — les deux rendaient « OVH » — et son appui redevenait un
         * interrupteur de presence pour ceux-la, exactement ce que la
         * decision 4 de la spec existe pour empecher. Ces deux paires sont la
         * raison pour laquelle le nom COMPLET est desormais affiche.
         */
        { "OVH:perso",         "OVH:pro"     },
        { "Ankama:perso",      "Ankama:pro"  },
        { "GitHub:mae",        "GitHub mae"  },   /* ponctuation vs espace */
        { "Banque",            "Ban\x01que"  },   /* caractere de controle */
        { "MonServiceTresLong1", "MonServiceTresLong2" }, /* divergence tardive */
        /* Meme prefixe visible APRES assainissement (deux octets de controle
         * distincts retombent tous deux sur « ? »), meme queue masquee : si
         * l'empreinte ne portait que sur la queue, ces deux noms rendraient
         * la meme chaine — c'est exactement le trou que corrige la ronde 1. */
        { "Ban\x01que" "XXXXXXXXXXXXXXXX", "Ban\x02que" "XXXXXXXXXXXXXXXX" },
        /* Ronde 3 : cible directement le poids de l'empreinte (Horner sur
         * l'issuer entier), pas seulement l'idee generale de collision. Meme
         * prefixe visible de OATH_NAME_DISPLAY_MAX-3 caracteres, meme dernier
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
        { "SERVICE1234567890ABXAX", "SERVICE1234567890ABXMX" },
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

/*
 * Troncature marquee : sans marqueur, deux noms qui divergent au-dela de la
 * coupe seraient indiscernables et l'appui redeviendrait aveugle.
 *
 * Les longueurs sont exprimees en OATH_NAME_DISPLAY_MAX et non en litteraux :
 * cette constante a deja bouge une fois (12 -> 22, apres qu'on a mesure la
 * VRAIE police de cette ligne), et des « 11 » en dur avaient alors fige un
 * budget qui n'existait plus.
 *
 * La borne exacte est verifiee des DEUX cotes : le plus long nom qui tient
 * entier ne porte pas de marqueur, celui d'un caractere de plus en porte un.
 * Verifier un seul cote laisserait passer une coupe decalee d'un.
 */
static void test_troncature_marquee(void)
{
    char out[OATH_NAME_DISPLAY_MAX];
    /* Vingt-deux caracteres : un de trop pour les vingt-et-un dessinables. */
    const char *n = "ServiceExtremementLong";
    TEST_ASSERT_EQ(strlen(n), OATH_NAME_DISPLAY_MAX, "le cas de reference deborde bien d'un");
    oath_name_display(n, (uint16_t)strlen(n), out, sizeof(out));
    TEST_ASSERT_EQ(strlen(out), OATH_NAME_DISPLAY_MAX - 1u,
                   "tout le budget dessinable est occupe");
    TEST_ASSERT(out[OATH_NAME_DISPLAY_MAX - 2u] == '?',
                "le marqueur de troncature termine la ligne");

    /* Vingt-et-un caracteres : le plus long qui tienne entier. */
    const char *pile = "ServiceExtremementLon";
    TEST_ASSERT_EQ(strlen(pile), OATH_NAME_DISPLAY_MAX - 1u, "exactement le budget");
    oath_name_display(pile, (uint16_t)strlen(pile), out, sizeof(out));
    TEST_ASSERT(strcmp(out, "SERVICEEXTREMEMENTLON") == 0,
                "vingt-et-un caracteres passent entiers, sans marqueur");

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
    /* Un nom reduit a son seul prefixe de periode : il ne reste rien a
     * montrer. Remplace l'ancien cas « ':' -> chaine vide », qui ne tenait que
     * par la regle « emetteur seul » — un ':' isole est desormais un nom d'un
     * caractere, imprimable, et s'affiche. */
    oath_name_display("30/", 3, out, sizeof(out));
    TEST_ASSERT(out[0] == '\0', "rien apres le prefixe de periode -> chaine vide");
    oath_name_display(":", 1, out, sizeof(out));
    TEST_ASSERT(strcmp(out, ":") == 0, "un ':' seul est un nom d'un caractere");
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
 * (sec_confirm_arm_named -> oath_name_display) : retrait d'un prefixe
 * numerique suivi de '/', assainissement, troncature. Une forme mal choisie
 * ressortirait tronquee ou vide, et
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
 * qui porterait un prefixe « 30/ » (ampute), ou qui
 * depasserait le budget de caracteres visibles (tronquee, avec une empreinte
 * a la place du dernier chiffre). Aucun de ces trois defauts ne se voit sur la
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
    TEST_RUN(test_nom_complet_affiche);
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
