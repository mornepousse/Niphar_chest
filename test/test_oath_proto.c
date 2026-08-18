#include "test_framework.h"
#include "oath_proto.h"
#include "apdu.h"
#include "sec_store.h"

#include <stdio.h>

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

/*
 * SELECT et CALCULATE ALL valent TOUS DEUX 0xA4 et ne se distinguent que par
 * P1/P2 (oath.py : select -> P1=04 ; CALCULATE_ALL -> P2=01). Un aiguillage
 * sur le seul INS repondrait un SELECT a une demande de codes. Le test compare
 * les deux reponses ENTRE ELLES : verifier separement que chacune « repond »
 * ne dirait rien sur le fait qu'elles different.
 */
static void test_select_et_calculate_all_ne_se_confondent_pas(void)
{
    /* Le magasin est un etat GLOBAL : sans cette remise a zero, ce cas
     * heriterait des slots du precedent et son resultat dependrait de
     * l'ordre d'execution. */
    sec_store_init();
    oath_ctx_t ctx; memset(&ctx, 0, sizeof(ctx));
    /* AID YKOATH complet (A0 00 00 05 27 21 01) : un Lc annonce a 7 avec
     * seulement trois octets de donnees serait refuse par apdu_parse comme
     * tronque, et le test ne testerait plus l'aiguillage mais l'analyseur. */
    uint8_t sel[12] = { 0x00, 0xA4, 0x04, 0x00, 0x07,
                        0xA0, 0x00, 0x00, 0x05, 0x27, 0x21, 0x01 };
    uint8_t all[5]  = { 0x00, 0xA4, 0x00, 0x01, 0x00 };
    uint8_t o1[256], o2[256];
    apdu_t a1, a2;

    TEST_ASSERT(apdu_parse(sel, sizeof(sel), &a1), "SELECT analyse");
    TEST_ASSERT(apdu_parse(all, sizeof(all), &a2), "CALCULATE ALL analyse");
    uint16_t n1 = oath_dispatch(&a1, o1, sizeof(o1), &ctx);
    uint16_t n2 = oath_dispatch(&a2, o2, sizeof(o2), &ctx);

    TEST_ASSERT(n1 != n2 || memcmp(o1, o2, n1) != 0,
                "SELECT et CALCULATE ALL ne rendent pas la meme chose");
    /* Precision de la garde ci-dessus : ce n'est pas seulement « different »,
     * c'est que la reponse aux codes ne doit porter AUCUN identifiant
     * d'applet. */
    const uint8_t *v = NULL; uint16_t vl = 0;
    TEST_ASSERT(!oath_tlv_find(o2, (uint16_t)(n2 - 2), OATH_TAG_VERSION, &v, &vl),
                "CALCULATE ALL ne rend pas la version de l'applet");
}

/* La reponse au SELECT DOIT porter 0x79 (version) : ykman fait
 * data[TAG_VERSION] sans garde et leve une exception s'il manque. */
static void test_select_porte_version_et_sel(void)
{
    sec_store_init();
    oath_ctx_t ctx; memset(&ctx, 0, sizeof(ctx));
    uint8_t sel[12] = { 0x00, 0xA4, 0x04, 0x00, 0x07,
                        0xA0, 0x00, 0x00, 0x05, 0x27, 0x21, 0x01 };
    uint8_t out[256]; apdu_t a;
    TEST_ASSERT(apdu_parse(sel, sizeof(sel), &a), "SELECT analyse");
    uint16_t n = oath_dispatch(&a, out, sizeof(out), &ctx);

    TEST_ASSERT(n > 2, "le SELECT rend des donnees, pas un mot d'etat seul");
    const uint8_t *v = NULL; uint16_t vl = 0;
    TEST_ASSERT(oath_tlv_find(out, (uint16_t)(n - 2), OATH_TAG_VERSION, &v, &vl),
                "0x79 present, sinon ykman leve une exception");
    TEST_ASSERT(oath_tlv_find(out, (uint16_t)(n - 2), OATH_TAG_NAME, &v, &vl),
                "0x71 present : _get_device_id() en fait un SHA-256");
    TEST_ASSERT(!oath_tlv_find(out, (uint16_t)(n - 2), OATH_TAG_CHALLENGE, &v, &vl),
                "0x74 absent : c'est ce qui signale « pas de mot de passe »");
    TEST_ASSERT(ctx.selected, "le SELECT arme l'applet");
}

/* Les commandes hors portee se refusent explicitement, jamais en silence. */
static void test_commandes_refusees(void)
{
    sec_store_init();
    oath_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.selected = true;
    const uint8_t ins_refuses[] = { 0x03, 0xA3 };
    for (unsigned i = 0; i < sizeof(ins_refuses); i++) {
        uint8_t cmd[5] = { 0x00, ins_refuses[i], 0x00, 0x00, 0x00 };
        uint8_t out[16]; apdu_t a;
        TEST_ASSERT(apdu_parse(cmd, sizeof(cmd), &a), "commande analysee");
        uint16_t n = oath_dispatch(&a, out, sizeof(out), &ctx);
        TEST_ASSERT_EQ(n, 2, "mot d'etat seul");
        TEST_ASSERT(out[0] == 0x6A && out[1] == 0x81, "6A81 : fonction non supportee");
    }
}

/* Applet non selectionne : toute commande OATH doit etre refusee. */
static void test_refus_si_non_selectionne(void)
{
    sec_store_init();
    oath_ctx_t ctx; memset(&ctx, 0, sizeof(ctx));   /* selected = false */
    uint8_t cmd[5] = { 0x00, 0xA1, 0x00, 0x00, 0x00 };   /* LIST */
    uint8_t out[16]; apdu_t a;
    TEST_ASSERT(apdu_parse(cmd, sizeof(cmd), &a), "LIST analyse");
    uint16_t n = oath_dispatch(&a, out, sizeof(out), &ctx);
    TEST_ASSERT_EQ(n, 2, "mot d'etat seul");
    TEST_ASSERT(out[0] == 0x6A && out[1] == 0x82, "6A82 tant que rien n'est selectionne");
}

/*
 * Douze comptes depassent 255 octets : LIST doit rendre 61xx et le reste par
 * SEND REMAINING. Une implementation qui l'omet marche avec trois comptes et
 * casse avec douze — exactement apres la migration.
 */
static void test_list_douze_comptes_decoupe(void)
{
    sec_store_init();
    oath_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.selected = true;
    const uint8_t key[20] = { 0 };
    for (uint8_t i = 0; i < 12; i++) {
        char nom[40];
        snprintf(nom, sizeof(nom), "ServiceNumero%02u:compte@exemple.org",
                 (unsigned)i);
        TEST_ASSERT(sec_store_set_slot(i, 0x21, nom, key, sizeof(key)),
                    "slot provisionne");
    }
    uint8_t cmd[5] = { 0x00, 0xA1, 0x00, 0x00, 0x00 };
    uint8_t out[300]; apdu_t a;
    TEST_ASSERT(apdu_parse(cmd, sizeof(cmd), &a), "LIST analyse");
    uint16_t n = oath_dispatch(&a, out, sizeof(out), &ctx);

    TEST_ASSERT(n > 2, "la premiere tranche porte des donnees");
    TEST_ASSERT(out[n - 2] == 0x61, "61xx : il reste des octets");
    TEST_ASSERT(ctx.pending_len > 0, "le reste est garde pour SEND REMAINING");

    /* Les octets rendus doivent etre la VRAIE liste, pas un remplissage : le
     * premier TLV porte l'etiquette de liste et le nom du premier compte. */
    TEST_ASSERT(out[0] == OATH_TAG_NAME_LIST, "0x72 en tete de LIST");
    TEST_ASSERT(out[2] == 0x21, "premier octet de valeur : type|algo du slot");
    TEST_ASSERT(memcmp(&out[3], "ServiceNumero00:", 16) == 0,
                "le nom du premier compte suit");

    const uint16_t premiere = (uint16_t)(n - 2);
    uint8_t suite[5] = { 0x00, 0xA5, 0x00, 0x00, 0x00 };
    TEST_ASSERT(apdu_parse(suite, sizeof(suite), &a), "SEND REMAINING analyse");
    uint16_t m = oath_dispatch(&a, out, sizeof(out), &ctx);
    TEST_ASSERT(m >= 2, "SEND REMAINING rend quelque chose");
    TEST_ASSERT(out[m - 2] == 0x90 && out[m - 1] == 0x00,
                "la derniere tranche se termine par 9000");
    /* Les deux tranches reunies doivent faire la liste ENTIERE : une decoupe
     * qui perd des octets au raccord passerait les deux gardes precedentes. */
    TEST_ASSERT_EQ((uint32_t)premiere + (uint32_t)(m - 2), 12u * 37u,
                   "douze entrees de 37 octets, aucune perdue au raccord");
    TEST_ASSERT_EQ(ctx.pending_len, 0, "le tampon differe est rendu au repos");
}

/* Un defi de longueur autre que huit est une trame malformee. */
static void test_defi_doit_faire_huit_octets(void)
{
    sec_store_init();
    oath_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.selected = true;
    const uint8_t key[20] = { 0 };
    TEST_ASSERT(sec_store_set_slot(0, 0x21, "Test:x", key, sizeof(key)),
                "slot provisionne");

    uint8_t data[] = { OATH_TAG_NAME, 0x06, 'T','e','s','t',':','x',
                       OATH_TAG_CHALLENGE, 0x04, 0,0,0,0 };
    uint8_t cmd[5 + sizeof(data)];
    cmd[0] = 0x00; cmd[1] = 0xA2; cmd[2] = 0x00; cmd[3] = 0x01;
    cmd[4] = (uint8_t)sizeof(data);
    memcpy(&cmd[5], data, sizeof(data));

    uint8_t out[64]; apdu_t a;
    TEST_ASSERT(apdu_parse(cmd, sizeof(cmd), &a), "CALCULATE analyse");
    uint16_t n = oath_dispatch(&a, out, sizeof(out), &ctx);
    TEST_ASSERT_EQ(n, 2, "mot d'etat seul");
    TEST_ASSERT(out[0] == 0x6A && out[1] == 0x80, "6A80 : defi de mauvaise longueur");

    /* Temoin positif : le MEME appel avec huit octets de defi doit, lui,
     * demander l'appui. Sans ce contraste, « toujours refuser » passerait
     * l'assertion ci-dessus. */
    uint8_t data8[] = { OATH_TAG_NAME, 0x06, 'T','e','s','t',':','x',
                        OATH_TAG_CHALLENGE, 0x08, 0,0,0,0,0,0,0,0 };
    uint8_t cmd8[5 + sizeof(data8)];
    cmd8[0] = 0x00; cmd8[1] = 0xA2; cmd8[2] = 0x00; cmd8[3] = 0x01;
    cmd8[4] = (uint8_t)sizeof(data8);
    memcpy(&cmd8[5], data8, sizeof(data8));
    TEST_ASSERT(apdu_parse(cmd8, sizeof(cmd8), &a), "CALCULATE 8 octets analyse");
    TEST_ASSERT_EQ(oath_dispatch(&a, out, sizeof(out), &ctx),
                   OATH_SW_NEEDS_TOUCH,
                   "un defi de huit octets demande l'appui physique");
}

/* Un nom inconnu ne doit pas armer un appui : sinon l'hote ferait clignoter
 * la clef pour un compte qui n'existe pas. */
static void test_calculate_nom_inconnu(void)
{
    sec_store_init();
    oath_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.selected = true;
    const uint8_t key[20] = { 0 };
    TEST_ASSERT(sec_store_set_slot(0, 0x21, "Test:x", key, sizeof(key)),
                "slot provisionne");

    uint8_t data[] = { OATH_TAG_NAME, 0x06, 'A','u','t','r','e','!',
                       OATH_TAG_CHALLENGE, 0x08, 0,0,0,0,0,0,0,0 };
    uint8_t cmd[5 + sizeof(data)];
    cmd[0] = 0x00; cmd[1] = 0xA2; cmd[2] = 0x00; cmd[3] = 0x01;
    cmd[4] = (uint8_t)sizeof(data);
    memcpy(&cmd[5], data, sizeof(data));

    uint8_t out[64]; apdu_t a;
    TEST_ASSERT(apdu_parse(cmd, sizeof(cmd), &a), "CALCULATE analyse");
    uint16_t n = oath_dispatch(&a, out, sizeof(out), &ctx);
    TEST_ASSERT_EQ(n, 2, "mot d'etat seul");
    TEST_ASSERT(out[0] == 0x6A && out[1] == 0x82, "6A82 : compte inconnu");
}

/*
 * PUT vient de l'hote : c'est par lui qu'entrent les longueurs. Un secret plus
 * long que le slot, un type HOTP, un magasin plein doivent se refuser AVANT
 * toute ecriture — un refus tardif aurait deja deborde.
 */
static void test_put_borne_les_entrees_hote(void)
{
    sec_store_init();
    oath_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.selected = true;
    uint8_t out[64]; apdu_t a;

    /* 1. TOTP/SHA1, six chiffres, secret de 20 octets : accepte. */
    uint8_t bon[2 + 4 + 2 + 2 + 20];
    uint16_t k = 0;
    bon[k++] = OATH_TAG_NAME; bon[k++] = 4;
    memcpy(&bon[k], "A:b", 3); bon[k + 3] = 'c'; k = (uint16_t)(k + 4);
    bon[k++] = OATH_TAG_KEY; bon[k++] = 22; bon[k++] = 0x21; bon[k++] = 6;
    memset(&bon[k], 0xAB, 20); k = (uint16_t)(k + 20);

    uint8_t cmd[5 + sizeof(bon)];
    cmd[0] = 0x00; cmd[1] = 0x01; cmd[2] = 0x00; cmd[3] = 0x00;
    cmd[4] = (uint8_t)k;
    memcpy(&cmd[5], bon, k);
    TEST_ASSERT(apdu_parse(cmd, (uint16_t)(5 + k), &a), "PUT analyse");
    uint16_t n = oath_dispatch(&a, out, sizeof(out), &ctx);
    TEST_ASSERT_EQ(n, 2, "mot d'etat seul");
    TEST_ASSERT(out[0] == 0x90 && out[1] == 0x00, "9000 : PUT accepte");
    TEST_ASSERT_EQ(sec_store_count(), 1, "un slot occupe");
    TEST_ASSERT_EQ(sec_store_digits(0), 6, "les chiffres viennent du TLV 0x73");

    /* 2. Meme trame, type HOTP (0x10) : refusee, et le magasin ne bouge pas. */
    cmd[5 + 6 + 2] = 0x11;      /* octet type|algo dans la valeur du 0x73 */
    TEST_ASSERT(apdu_parse(cmd, (uint16_t)(5 + k), &a), "PUT HOTP analyse");
    n = oath_dispatch(&a, out, sizeof(out), &ctx);
    TEST_ASSERT_EQ(n, 2, "mot d'etat seul");
    TEST_ASSERT(out[0] == 0x6A && out[1] == 0x81, "6A81 : HOTP hors portee");
    TEST_ASSERT_EQ(sec_store_count(), 1, "le magasin n'a pas bouge");

    /* 3. Secret de 65 octets : un de plus que le slot. Doit se refuser sans
     *    rien ecrire — c'est le debordement que cette borne existe pour
     *    empecher. */
    uint8_t trop[2 + 4 + 2 + 2 + 65];
    k = 0;
    trop[k++] = OATH_TAG_NAME; trop[k++] = 4;
    memcpy(&trop[k], "A:bd", 4); k = (uint16_t)(k + 4);
    trop[k++] = OATH_TAG_KEY; trop[k++] = 67; trop[k++] = 0x21; trop[k++] = 6;
    memset(&trop[k], 0xCD, 65); k = (uint16_t)(k + 65);
    uint8_t cmd2[5 + sizeof(trop)];
    cmd2[0] = 0x00; cmd2[1] = 0x01; cmd2[2] = 0x00; cmd2[3] = 0x00;
    cmd2[4] = (uint8_t)k;
    memcpy(&cmd2[5], trop, k);
    TEST_ASSERT(apdu_parse(cmd2, (uint16_t)(5 + k), &a), "PUT long analyse");
    n = oath_dispatch(&a, out, sizeof(out), &ctx);
    TEST_ASSERT_EQ(n, 2, "mot d'etat seul");
    TEST_ASSERT(out[0] == 0x6A && out[1] == 0x80, "6A80 : secret trop long");
    TEST_ASSERT_EQ(sec_store_count(), 1, "aucun slot n'a ete ecrit");
}

/* Magasin plein : le seizieme slot pris, le dix-septieme compte doit se
 * refuser par 6A84 plutot que d'ecraser un compte au hasard. */
static void test_put_magasin_plein(void)
{
    sec_store_init();
    oath_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.selected = true;
    const uint8_t key[20] = { 0 };
    for (uint8_t i = 0; i < SEC_N_SLOTS; i++) {
        char nom[16];
        snprintf(nom, sizeof(nom), "S%02u:c", (unsigned)i);
        TEST_ASSERT(sec_store_set_slot(i, 0x21, nom, key, sizeof(key)),
                    "slot provisionne");
    }
    uint8_t d[] = { OATH_TAG_NAME, 0x05, 'Z','Z','Z',':','c',
                    OATH_TAG_KEY, 0x04, 0x21, 6, 0x01, 0x02 };
    uint8_t cmd[5 + sizeof(d)];
    cmd[0] = 0x00; cmd[1] = 0x01; cmd[2] = 0x00; cmd[3] = 0x00;
    cmd[4] = (uint8_t)sizeof(d);
    memcpy(&cmd[5], d, sizeof(d));
    uint8_t out[16]; apdu_t a;
    TEST_ASSERT(apdu_parse(cmd, sizeof(cmd), &a), "PUT analyse");
    uint16_t n = oath_dispatch(&a, out, sizeof(out), &ctx);
    TEST_ASSERT_EQ(n, 2, "mot d'etat seul");
    TEST_ASSERT(out[0] == 0x6A && out[1] == 0x84, "6A84 : magasin plein");
    TEST_ASSERT_EQ(sec_store_count(), SEC_N_SLOTS, "aucun compte ecrase");
}

/* DELETE et RESET : ce qui disparait doit vraiment disparaitre, et un nom
 * inconnu ne doit pas passer pour une suppression reussie. */
static void test_delete_et_reset(void)
{
    sec_store_init();
    oath_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.selected = true;
    const uint8_t key[20] = { 0 };
    TEST_ASSERT(sec_store_set_slot(0, 0x21, "A:b", key, sizeof(key)), "slot 0");
    TEST_ASSERT(sec_store_set_slot(1, 0x21, "C:d", key, sizeof(key)), "slot 1");

    uint8_t d[] = { OATH_TAG_NAME, 0x03, 'A', ':', 'b' };
    uint8_t cmd[5 + sizeof(d)];
    cmd[0] = 0x00; cmd[1] = 0x02; cmd[2] = 0x00; cmd[3] = 0x00;
    cmd[4] = (uint8_t)sizeof(d);
    memcpy(&cmd[5], d, sizeof(d));
    uint8_t out[16]; apdu_t a;
    TEST_ASSERT(apdu_parse(cmd, sizeof(cmd), &a), "DELETE analyse");
    uint16_t n = oath_dispatch(&a, out, sizeof(out), &ctx);
    TEST_ASSERT(n == 2 && out[0] == 0x90 && out[1] == 0x00, "9000 : supprime");
    TEST_ASSERT_EQ(sec_store_count(), 1, "il reste un compte");

    /* Le meme DELETE une seconde fois : le nom n'existe plus. */
    n = oath_dispatch(&a, out, sizeof(out), &ctx);
    TEST_ASSERT(n == 2 && out[0] == 0x6A && out[1] == 0x82,
                "6A82 : deux suppressions du meme nom ne reussissent pas deux fois");

    uint8_t rst[5] = { 0x00, 0x04, 0xDE, 0xAD, 0x00 };
    TEST_ASSERT(apdu_parse(rst, sizeof(rst), &a), "RESET analyse");
    n = oath_dispatch(&a, out, sizeof(out), &ctx);
    TEST_ASSERT(n == 2 && out[0] == 0x90 && out[1] == 0x00, "9000 : reset");
    TEST_ASSERT_EQ(sec_store_count(), 0, "le magasin est vide");
}

/* RENAME porte DEUX TLV 0x71 de suite : lire le premier deux fois renommerait
 * un compte en lui-meme, sans que rien ne le signale. */
static void test_rename_lit_le_second_nom(void)
{
    sec_store_init();
    oath_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.selected = true;
    const uint8_t key[20] = { 0 };
    TEST_ASSERT(sec_store_set_slot(0, 0x21, "A:b", key, sizeof(key)), "slot 0");

    uint8_t d[] = { OATH_TAG_NAME, 0x03, 'A', ':', 'b',
                    OATH_TAG_NAME, 0x03, 'X', ':', 'y' };
    uint8_t cmd[5 + sizeof(d)];
    cmd[0] = 0x00; cmd[1] = 0x05; cmd[2] = 0x00; cmd[3] = 0x00;
    cmd[4] = (uint8_t)sizeof(d);
    memcpy(&cmd[5], d, sizeof(d));
    uint8_t out[16]; apdu_t a;
    TEST_ASSERT(apdu_parse(cmd, sizeof(cmd), &a), "RENAME analyse");
    uint16_t n = oath_dispatch(&a, out, sizeof(out), &ctx);
    TEST_ASSERT(n == 2 && out[0] == 0x90 && out[1] == 0x00, "9000 : renomme");
    TEST_ASSERT(sec_store_label(0) != NULL && strcmp(sec_store_label(0), "X:y") == 0,
                "l'etiquette porte le SECOND nom, pas le premier");
    TEST_ASSERT_EQ(sec_store_count(), 1, "renommer ne cree pas de compte");
}

/* CALCULATE ALL doit annoncer chaque compte comme exigeant un appui (0x7C) et
 * ne JAMAIS rendre de code : un code rendu sans appui viderait la clef de son
 * seul controle physique. */
static void test_calculate_all_ne_rend_aucun_code(void)
{
    sec_store_init();
    oath_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.selected = true;
    const uint8_t key[20] = { 0 };
    TEST_ASSERT(sec_store_set_slot(0, 0x21, "A:b", key, sizeof(key)), "slot 0");

    uint8_t cmd[5] = { 0x00, 0xA4, 0x00, 0x01, 0x00 };
    uint8_t out[256]; apdu_t a;
    TEST_ASSERT(apdu_parse(cmd, sizeof(cmd), &a), "CALCULATE ALL analyse");
    uint16_t n = oath_dispatch(&a, out, sizeof(out), &ctx);
    TEST_ASSERT(n > 2, "un compte present, donc des donnees");

    const uint16_t body = (uint16_t)(n - 2);
    const uint8_t *v = NULL; uint16_t vl = 0;
    TEST_ASSERT(oath_tlv_find(out, body, OATH_TAG_NAME, &v, &vl), "0x71 present");
    TEST_ASSERT(vl == 3 && memcmp(v, "A:b", 3) == 0, "le nom du compte");
    TEST_ASSERT(oath_tlv_find(out, body, OATH_TAG_TOUCH, &v, &vl),
                "0x7C : appui exige");
    TEST_ASSERT(!oath_tlv_find(out, body, OATH_TAG_RESPONSE, &v, &vl),
                "aucun 0x75 : pas de code sans appui");
    TEST_ASSERT(!oath_tlv_find(out, body, OATH_TAG_TRUNCATED, &v, &vl),
                "aucun 0x76 : pas de code tronque non plus");
}

/*
 * Une capacite de sortie trop petite ne doit jamais faire ecrire au-dela :
 * `cap` vient de la couche CCID, pas d'une constante de ce fichier.
 */
static void test_capacite_de_sortie_respectee(void)
{
    sec_store_init();
    oath_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.selected = true;
    const uint8_t key[20] = { 0 };
    for (uint8_t i = 0; i < 12; i++) {
        char nom[40];
        snprintf(nom, sizeof(nom), "ServiceNumero%02u:compte@exemple.org",
                 (unsigned)i);
        TEST_ASSERT(sec_store_set_slot(i, 0x21, nom, key, sizeof(key)), "slot");
    }
    uint8_t cmd[5] = { 0x00, 0xA1, 0x00, 0x00, 0x00 };
    /* Tampon suivi d'une zone temoin : si oath_dispatch ecrit au-dela de
     * `cap`, le temoin change. */
    uint8_t zone[64];
    memset(zone, 0x5A, sizeof(zone));
    apdu_t a;
    TEST_ASSERT(apdu_parse(cmd, sizeof(cmd), &a), "LIST analyse");
    uint16_t n = oath_dispatch(&a, zone, 32, &ctx);
    TEST_ASSERT(n <= 32, "la reponse tient dans la capacite annoncee");
    for (unsigned i = 32; i < sizeof(zone); i++)
        TEST_ASSERT(zone[i] == 0x5A, "rien d'ecrit au-dela de cap");

    /* Capacite d'un seul octet : meme un mot d'etat n'y tient pas. Rendre 0
     * est le seul comportement qui n'ecrit rien. */
    memset(zone, 0x5A, sizeof(zone));
    TEST_ASSERT_EQ(oath_dispatch(&a, zone, 1, &ctx), 0, "cap<2 -> rien d'ecrit");
    TEST_ASSERT(zone[0] == 0x5A, "premier octet intact");
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
    TEST_RUN(test_select_et_calculate_all_ne_se_confondent_pas);
    TEST_RUN(test_select_porte_version_et_sel);
    TEST_RUN(test_commandes_refusees);
    TEST_RUN(test_refus_si_non_selectionne);
    TEST_RUN(test_list_douze_comptes_decoupe);
    TEST_RUN(test_defi_doit_faire_huit_octets);
    TEST_RUN(test_calculate_nom_inconnu);
    TEST_RUN(test_put_borne_les_entrees_hote);
    TEST_RUN(test_put_magasin_plein);
    TEST_RUN(test_delete_et_reset);
    TEST_RUN(test_rename_lit_le_second_nom);
    TEST_RUN(test_calculate_all_ne_rend_aucun_code);
    TEST_RUN(test_capacite_de_sortie_respectee);
}
