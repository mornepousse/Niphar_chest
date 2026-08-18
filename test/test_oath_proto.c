#include "test_framework.h"
#include "oath_proto.h"

/* Pour le test de debordement au sommet de uint16_t : page memoire protegee
 * (technique hote, POSIX) plutot qu'un octet sentinelle — voir le
 * commentaire de test_tlv_find_ne_lit_pas_au_dela_du_sommet_uint16. */
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <unistd.h>

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

/*
 * Trouvaille de revue : `hmac_len < 20` rend 0 dans l'implementation, mais
 * rien ne l'exercait — une garde que rien n'exerce est une garde dont
 * personne ne sait si elle marche. On compare les resultats ENTRE EUX, pas
 * seulement chacun a une constante : 0 et 19 doivent refuser IDENTIQUEMENT
 * (meme valeur), et 20 doit s'en distinguer par un vrai calcul, pas par
 * coincidence.
 */
static void test_hmac_len_garde_minimale(void)
{
    uint8_t h[20];
    for (unsigned i = 0; i < sizeof(h); i++) h[i] = (uint8_t)(i + 1);

    const uint32_t r0  = oath_dynamic_binary(h, 0);
    const uint32_t r19 = oath_dynamic_binary(h, 19);
    const uint32_t r20 = oath_dynamic_binary(h, 20);

    TEST_ASSERT_EQ(r0, r19, "hmac_len=0 et hmac_len=19 refusent identiquement");
    TEST_ASSERT_EQ(r0, 0u, "en dessous du minimum (20), le code dynamique est 0");
    TEST_ASSERT(r20 != r0, "hmac_len=20 calcule reellement, distinct du refus");
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

static sigjmp_buf s_oob_jmp;
static void oob_handler(int sig)
{
    (void)sig;
    siglongjmp(s_oob_jmp, 1);
}

/*
 * Trouvaille de revue : la garde de boucle d'oath_tlv_find retronquait
 * `i + 2u` en uint16_t AVANT de comparer a `len`. Avec len=65535 et i porte
 * a 65534 par des entrees valides, (uint16_t)(65534+2) = (uint16_t)65536 = 0,
 * et « 0 <= 65535 » est vrai a tort : la boucle lit alors buf[i+1], un octet
 * au-dela du tampon LOGIQUE.
 *
 * Un simple octet sentinelle ne suffit PAS a faire mordre ce test : la garde
 * interne (deja correcte, `(uint32_t)i + 2u + l > len`) rattrape TOUJOURS ce
 * cas avant de rendre un resultat — parce que i+2 (65536 ou 65537) depasse
 * deja tout `len` representable en uint16_t (max 65535), quelle que soit la
 * valeur du sentinelle. Verifie par simulation exhaustive de l'arithmetique
 * (256 valeurs de sentinelle x 2 valeurs de len x 2 valeurs de i : aucun
 * contre-exemple). La valeur de retour ne peut donc JAMAIS trahir ce bug —
 * seule la lecture memoire elle-meme est fautive. On place donc le tampon
 * juste avant une page non mappee (PROT_NONE) : la lecture en trop
 * declenche un SIGSEGV deterministe, capture ici en echec de test plutot
 * qu'en crash du process.
 */
static void test_tlv_find_ne_lit_pas_au_dela_du_sommet_uint16(void)
{
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    const uint16_t len = 65535;

    /* `len` (65535) depasse largement une page (4096 sur cette machine) : il
     * faut assez de pages LISIBLES pour loger tout le tampon, plus UNE page
     * de garde a la suite. */
    const size_t data_pages = (page + (size_t)len - 1u) / page;
    const size_t total = (data_pages + 1u) * page;

    uint8_t *region = mmap(NULL, total, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT(region != MAP_FAILED, "mmap de la zone de test");
    if (region == MAP_FAILED) return;
    TEST_ASSERT(mprotect(region + data_pages * page, page, PROT_NONE) == 0,
                "page de garde rendue non lisible (sentinelle)");

    /* Le tampon se termine exactement au bord de la page de garde : son
     * dernier octet valide (buf[len-1]) est le dernier octet lisible avant
     * la page protegee — buf[len] tomberait dedans. */
    uint8_t *buf = region + data_pages * page - len;
    /* Boucle de remplissage en uint32_t, PAS en uint16_t : le meme piege que
     * celui teste ici (retronquer i+2 avant de comparer) s'appliquerait
     * sinon a cette boucle elle-meme — verifie a la main pendant la mise au
     * point de ce test, ou une premiere version bornee en uint16_t
     * debordait ici, avant meme d'atteindre oath_tlv_find. */
    for (uint32_t i = 0; i + 2u <= (uint32_t)(len - 1u); i += 2u) {
        buf[i] = 0x01;      /* tag quelconque, jamais celui recherche */
        buf[i + 1] = 0x00;  /* longueur nulle : avance de 2 a chaque entree */
    }
    buf[len - 1] = 0xAB;    /* octet isole en fin de tampon, trop court pour
                              * former une paire complete — la garde correcte
                              * doit s'arreter sans le lire comme un tag. */

    struct sigaction sa;
    struct sigaction old_sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = oob_handler;
    sigaction(SIGSEGV, &sa, &old_sa);

    bool crashed = false;
    if (sigsetjmp(s_oob_jmp, 1) == 0) {
        const uint8_t *v = NULL;
        uint16_t n = 0;
        (void)oath_tlv_find(buf, len, 0xAB, &v, &n);
    } else {
        crashed = true;
    }
    sigaction(SIGSEGV, &old_sa, NULL);
    munmap(region, total);

    TEST_ASSERT(!crashed,
        "aucune lecture au-dela du tampon logique — i+2 ne se retronque pas avant comparaison");
}

void test_oath_proto(void)
{
    TEST_SUITE("oath_proto");
    TEST_RUN(test_troncature_rfc4226);
    TEST_RUN(test_hmac_len_garde_minimale);
    TEST_RUN(test_offset_lu_du_dernier_quartet);
    TEST_RUN(test_tlv_trouve_et_absent);
    TEST_RUN(test_tlv_longueur_qui_deborde);
    TEST_RUN(test_tlv_put_borne);
    TEST_RUN(test_tlv_find_ne_lit_pas_au_dela_du_sommet_uint16);
}
