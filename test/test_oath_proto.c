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

    TEST_ASSERT(n1 >= 2 && n2 >= 2, "les deux reponses portent au moins un mot d'etat");
    if (n1 < 2 || n2 < 2) return;
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

/* ---- petits echafaudages, pour que les cas disent le protocole et non la
 * fabrication d'APDU ---------------------------------------------------- */

/* Fabrique une APDU courte et l'aiguille. */
static uint16_t oath_cmd_cla(oath_ctx_t *ctx, uint8_t cla, uint8_t ins,
                             uint8_t p1, uint8_t p2,
                             const uint8_t *data, uint8_t lc,
                             uint8_t *out, uint16_t cap)
{
    uint8_t buf[5 + 255];
    uint16_t len;
    apdu_t a;
    buf[0] = cla; buf[1] = ins; buf[2] = p1; buf[3] = p2;
    if (lc == 0) { buf[4] = 0x00; len = 5; }
    else { buf[4] = lc; memcpy(&buf[5], data, lc); len = (uint16_t)(5 + lc); }
    if (!apdu_parse(buf, len, &a)) {
        TEST_ASSERT(false, "APDU de test analysable");
        return 0;
    }
    return oath_dispatch(&a, out, cap, ctx);
}

static uint16_t oath_cmd(oath_ctx_t *ctx, uint8_t ins, uint8_t p1, uint8_t p2,
                         const uint8_t *data, uint8_t lc,
                         uint8_t *out, uint16_t cap)
{
    return oath_cmd_cla(ctx, 0x00, ins, p1, p2, data, lc, out, cap);
}

/* memmem est une extension GNU : on l'evite pour que le harnais reste
 * compilable partout. */
static bool bytes_contain(const uint8_t *h, uint16_t hn, const char *n, uint16_t nn)
{
    if (nn == 0 || hn < nn) return false;
    for (uint16_t i = 0; i + nn <= hn; i++)
        if (memcmp(&h[i], n, nn) == 0) return true;
    return false;
}

static bool sw_is(const uint8_t *out, uint16_t n, uint8_t hi, uint8_t lo)
{
    return n == 2 && out[0] == hi && out[1] == lo;
}

static uint8_t tlv_at(uint8_t *b, uint8_t at, uint8_t tag, const void *v, uint8_t l)
{
    b[at] = tag; b[at + 1] = l;
    if (l) memcpy(&b[at + 2], v, l);
    return (uint8_t)(at + 2 + l);
}

/* AID YKOATH complet — celui qu'envoie ykman. */
static const uint8_t k_aid[7] = { 0xA0, 0x00, 0x00, 0x05, 0x27, 0x21, 0x01 };

/* Selectionne l'applet pour de vrai, avec un sel reconnaissable. */
static void oath_select_ok(oath_ctx_t *ctx)
{
    uint8_t out[64];
    memset(ctx, 0, sizeof(*ctx));
    for (unsigned i = 0; i < OATH_SALT_LEN; i++) ctx->salt[i] = (uint8_t)(0xA0 + i);
    uint16_t n = oath_cmd(ctx, 0xA4, 0x04, 0x00, k_aid, sizeof(k_aid), out, sizeof(out));
    TEST_ASSERT(n > 2 && ctx->selected, "applet selectionne");
}

/* Provisionne un compte TOTP/SHA1 valide. */
static void slot_totp(uint8_t idx, const char *nom)
{
    const uint8_t key[20] = { 0 };
    TEST_ASSERT(sec_store_set_slot(idx, OATH_ALGO_TOTP_SHA1, nom, key, sizeof(key)),
                "slot TOTP provisionne");
}

/* Meme chose, avec un secret reconnaissable : c'est ce qui permet de dire
 * QUEL compte a bouge, et pas seulement combien il en reste. */
static void slot_totp_secret(uint8_t idx, const char *nom, uint8_t remplissage)
{
    uint8_t key[20];
    memset(key, remplissage, sizeof(key));
    TEST_ASSERT(sec_store_set_slot(idx, OATH_ALGO_TOTP_SHA1, nom, key, sizeof(key)),
                "slot TOTP provisionne");
}

/* Le slot porte-t-il toujours ce nom et ce secret ? */
static bool slot_intact(uint8_t idx, const char *nom, uint8_t remplissage)
{
    const char *lab = sec_store_label(idx);
    if (lab == NULL || strcmp(lab, nom) != 0) return false;
    uint8_t sec[SEC_SECRET_MAX]; uint8_t sl = 0;
    if (!sec_store_get_secret(idx, sec, &sl)) return false;
    if (sl != 20) return false;
    for (unsigned i = 0; i < 20; i++) if (sec[i] != remplissage) return false;
    return true;
}

/* Fabrique le corps d'un PUT : nom, puis 0x73 = [type][chiffres][secret]. */
static uint8_t put_body(uint8_t *d, const char *nom, uint8_t nom_len,
                        uint8_t digits, uint8_t remplissage, uint8_t secret_len)
{
    uint8_t clef[2 + SEC_SECRET_MAX];
    clef[0] = OATH_ALGO_TOTP_SHA1;
    clef[1] = digits;
    memset(&clef[2], remplissage, secret_len);
    uint8_t l = tlv_at(d, 0, OATH_TAG_NAME, nom, nom_len);
    return tlv_at(d, l, OATH_TAG_KEY, clef, (uint8_t)(2 + secret_len));
}

/*
 * C1 — le magasin est PARTAGE avec le mode OTP : otp_hid.c mappe les slots 0
 * et 1 sur les secrets CR-HMAC de KeePassXC. Un slot CR-HMAC doit etre
 * INVISIBLE a l'applet OATH, sans quoi l'hote le liste, l'efface, ou pire :
 * fait signer un defi de huit octets qu'il choisit par cette clef-la.
 * ykman le refuserait de toute facon — oath.py fait OATH_TYPE(0xF0 & data[0])
 * et leve sur 0x01.
 */
static void test_slots_cr_hmac_invisibles_a_oath(void)
{
    sec_store_init();
    oath_ctx_t ctx;
    const uint8_t key[20] = { 0 };
    TEST_ASSERT(sec_store_set_slot(0, SEC_SLOT_HMAC_SHA1, "keepassxc", key, sizeof(key)),
                "slot CR-HMAC provisionne");
    slot_totp(1, "A:b");
    oath_select_ok(&ctx);

    uint8_t out[256];
    uint16_t n = oath_cmd(&ctx, 0xA1, 0x00, 0x00, NULL, 0, out, sizeof(out));
    TEST_ASSERT(n > 2, "LIST rend le compte OATH");
    TEST_ASSERT(!bytes_contain(out, n, "keepassxc", 9),
                "LIST ne remonte pas le slot CR-HMAC");
    TEST_ASSERT(bytes_contain(out, n, "A:b", 3), "LIST remonte bien le compte OATH");

    n = oath_cmd(&ctx, 0xA4, 0x00, 0x01, NULL, 0, out, sizeof(out));
    TEST_ASSERT(!bytes_contain(out, n, "keepassxc", 9),
                "CALCULATE ALL ne remonte pas le slot CR-HMAC");

    uint8_t d[32];
    uint8_t l = tlv_at(d, 0, OATH_TAG_NAME, "keepassxc", 9);
    n = oath_cmd(&ctx, 0x02, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x6A, 0x82), "DELETE d'un slot CR-HMAC : inconnu");
    TEST_ASSERT_EQ(sec_store_type(0), SEC_SLOT_HMAC_SHA1, "le secret CR-HMAC survit");

    /* CALCULATE : c'est la sortie partielle qui compte, pas seulement la
     * suppression — un defi de huit octets choisi par l'hote ne doit jamais
     * atteindre la clef CR-HMAC. */
    l = tlv_at(d, 0, OATH_TAG_NAME, "keepassxc", 9);
    { const uint8_t c8[8] = { 0 }; l = tlv_at(d, l, OATH_TAG_CHALLENGE, c8, 8); }
    n = oath_cmd(&ctx, 0xA2, 0x00, 0x01, d, l, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x6A, 0x82),
                "CALCULATE sur un slot CR-HMAC : inconnu, jamais d'appui arme");

    /* RESET confirme : il efface les comptes OATH, pas le reste du magasin. */
    n = oath_cmd(&ctx, 0x04, 0xDE, 0xAD, NULL, 0, out, sizeof(out));
    TEST_ASSERT_EQ(n, OATH_SW_NEEDS_TOUCH, "RESET demande l'appui");
    n = oath_touch_commit(&ctx, true, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x90, 0x00), "RESET confirme");
    TEST_ASSERT_EQ(sec_store_type(0), SEC_SLOT_HMAC_SHA1,
                   "RESET n'a pas touche au secret CR-HMAC");
    TEST_ASSERT_EQ(sec_store_type(1), SEC_SLOT_EMPTY, "le compte OATH a disparu");
}

/*
 * C2 — oath.py:318 envoie RESET avec P1=0xDE, P2=0xAD. Ces deux octets SONT
 * le verrou : c'est leur seule raison d'etre. Un RESET qui ne les lit pas
 * efface le magasin sur une trame de quatre octets.
 */
static void test_reset_exige_de_ad(void)
{
    sec_store_init();
    oath_ctx_t ctx;
    slot_totp(0, "A:b");
    oath_select_ok(&ctx);
    uint8_t out[32];

    const uint8_t mauvais[][2] = { { 0x00, 0x00 }, { 0xDE, 0x00 }, { 0x00, 0xAD },
                                   { 0xAD, 0xDE } };
    for (unsigned i = 0; i < sizeof(mauvais) / sizeof(mauvais[0]); i++) {
        uint16_t n = oath_cmd(&ctx, 0x04, mauvais[i][0], mauvais[i][1], NULL, 0,
                              out, sizeof(out));
        TEST_ASSERT(sw_is(out, n, 0x6A, 0x80), "6A80 : verrou DE/AD absent");
        TEST_ASSERT_EQ(sec_store_count(), 1, "le magasin survit a un RESET sans verrou");
        TEST_ASSERT_EQ(ctx.touch_op, OATH_TOUCH_NONE, "aucun appui arme");
    }

    uint16_t n = oath_cmd(&ctx, 0x04, 0xDE, 0xAD, NULL, 0, out, sizeof(out));
    TEST_ASSERT_EQ(n, OATH_SW_NEEDS_TOUCH, "DE/AD : la commande est recevable");
}

/*
 * Decision de revue : RESET et DELETE detruisent des secrets, donc exigent le
 * meme geste physique que CALCULATE. Le code d'operation doit les distinguer —
 * l'ecran dira « EFFACER » et non « CODE OTP ».
 */
static void test_delete_et_reset_exigent_l_appui(void)
{
    sec_store_init();
    oath_ctx_t ctx;
    slot_totp(0, "A:b");
    slot_totp(1, "C:d");
    oath_select_ok(&ctx);
    uint8_t out[32], d[16];

    uint8_t l = tlv_at(d, 0, OATH_TAG_NAME, "A:b", 3);
    uint16_t n = oath_cmd(&ctx, 0x02, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT_EQ(n, OATH_SW_NEEDS_TOUCH, "DELETE demande l'appui");
    TEST_ASSERT_EQ(ctx.touch_op, OATH_TOUCH_DELETE, "operation distincte de CALCULATE");
    TEST_ASSERT_EQ(ctx.touch_slot, 0, "le slot vise est retenu");
    TEST_ASSERT_EQ(sec_store_count(), 2, "rien n'est efface avant l'appui");

    /* Appui refuse : le compte doit survivre. */
    n = oath_touch_commit(&ctx, false, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x69, 0x85), "6985 : appui refuse");
    TEST_ASSERT_EQ(sec_store_count(), 2, "un refus n'efface rien");
    TEST_ASSERT_EQ(ctx.touch_op, OATH_TOUCH_NONE, "la demande est retiree");
    /* Invariant : rien en attente, donc rien a afficher. Un compte residuel
     * ferait annoncer « EFFACER 12 COMPTES » alors qu'aucune demande ne
     * court. */
    TEST_ASSERT_EQ(ctx.touch_count, 0, "aucun compte annonce sans demande en cours");

    /* Et une confirmation qui ne suit aucune demande n'efface rien non plus. */
    n = oath_touch_commit(&ctx, true, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x69, 0x85), "6985 : rien en attente");
    TEST_ASSERT_EQ(sec_store_count(), 2, "aucun effacement sans demande");

    n = oath_cmd(&ctx, 0x02, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT_EQ(n, OATH_SW_NEEDS_TOUCH, "DELETE re-demande l'appui");
    n = oath_touch_commit(&ctx, true, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x90, 0x00), "9000 : supprime apres appui");
    TEST_ASSERT_EQ(sec_store_count(), 1, "il reste un compte");
    TEST_ASSERT_EQ(ctx.touch_count, 0, "plus rien a annoncer une fois l'effacement fait");

    /* Un nom inconnu se refuse AVANT l'appui : faire clignoter la clef pour
     * un compte inexistant apprendrait a confirmer sans regarder. */
    n = oath_cmd(&ctx, 0x02, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x6A, 0x82), "6A82 : nom deja supprime");
    TEST_ASSERT_EQ(ctx.touch_op, OATH_TOUCH_NONE, "aucun appui arme pour rien");

    /* RESET, meme exigence. */
    n = oath_cmd(&ctx, 0x04, 0xDE, 0xAD, NULL, 0, out, sizeof(out));
    TEST_ASSERT_EQ(n, OATH_SW_NEEDS_TOUCH, "RESET demande l'appui");
    TEST_ASSERT_EQ(ctx.touch_op, OATH_TOUCH_RESET, "operation RESET distincte");
    TEST_ASSERT_EQ(sec_store_count(), 1, "rien n'est efface avant l'appui");
    n = oath_touch_commit(&ctx, false, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x69, 0x85), "6985 : RESET refuse");
    TEST_ASSERT_EQ(sec_store_count(), 1, "le magasin survit au refus");
    n = oath_cmd(&ctx, 0x04, 0xDE, 0xAD, NULL, 0, out, sizeof(out));
    TEST_ASSERT_EQ(n, OATH_SW_NEEDS_TOUCH, "RESET re-demande l'appui");
    n = oath_touch_commit(&ctx, true, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x90, 0x00), "9000 : magasin remis a neuf");
    TEST_ASSERT_EQ(sec_store_count(), 0, "le magasin est vide");
}

/*
 * I1 — ces trois champs decident QUEL secret sort apres l'appui. Ils sont
 * l'unique sortie utile de CALCULATE, et rien ne les observait : trois
 * mutations (slot fige a 0, defi non recopie, defi inverse) survivaient.
 */
static void test_calculate_renseigne_le_contexte_d_appui(void)
{
    sec_store_init();
    oath_ctx_t ctx;
    /* Slot 3, pas 0 : un « touch_slot = 0 » code en dur passerait sur le slot 0. */
    slot_totp(0, "Zero:z");
    slot_totp(3, "Trois:t");
    oath_select_ok(&ctx);

    const uint8_t defi[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
    uint8_t d[32], out[32];
    uint8_t l = tlv_at(d, 0, OATH_TAG_NAME, "Trois:t", 7);
    l = tlv_at(d, l, OATH_TAG_CHALLENGE, defi, 8);

    uint16_t n = oath_cmd(&ctx, 0xA2, 0x00, 0x01, d, l, out, sizeof(out));
    TEST_ASSERT_EQ(n, OATH_SW_NEEDS_TOUCH, "l'appui est demande");
    TEST_ASSERT_EQ(ctx.touch_op, OATH_TOUCH_CALCULATE, "operation CALCULATE");
    TEST_ASSERT_EQ(ctx.touch_slot, 3, "le slot vise est le bon, pas le premier");
    /* Octet par octet, et dans l'ORDRE : un defi recopie a l'envers donnerait
     * un code d'un tout autre pas de temps. */
    for (unsigned i = 0; i < 8; i++)
        TEST_ASSERT_EQ(ctx.touch_challenge[i], defi[i], "octet du defi, dans l'ordre");
    TEST_ASSERT(ctx.touch_truncate, "P2=01 : ykman veut un 0x76 tronque");

    /* P2=00 : reponse complete. Le champ doit suivre P2, pas rester fige. */
    memset(&ctx.touch_challenge, 0, sizeof(ctx.touch_challenge));
    n = oath_cmd(&ctx, 0xA2, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT_EQ(n, OATH_SW_NEEDS_TOUCH, "l'appui est demande");
    TEST_ASSERT(!ctx.touch_truncate, "P2=00 : reponse complete, 0x75");
    TEST_ASSERT_EQ(ctx.touch_challenge[0], 0x11, "le defi est bien recopie a chaque fois");
}

/*
 * I2 — le tampon differe survivait a DELETE, PUT et RENAME : apres un LIST
 * puis une suppression, SEND REMAINING servait encore le compte supprime.
 */
static void test_pending_purge_par_chaque_mutation(void)
{
    uint8_t out[300], d[96];

    /* --- DELETE --- */
    sec_store_init();
    oath_ctx_t ctx;
    for (uint8_t i = 0; i < 12; i++) {
        char nom[40];
        snprintf(nom, sizeof(nom), "ServiceNumero%02u:compte@exemple.org", (unsigned)i);
        slot_totp(i, nom);
    }
    oath_select_ok(&ctx);
    uint16_t n = oath_cmd(&ctx, 0xA1, 0x00, 0x00, NULL, 0, out, sizeof(out));
    TEST_ASSERT(out[n - 2] == 0x61 && ctx.pending_len > 0, "un reste est en attente");

    uint8_t l = tlv_at(d, 0, OATH_TAG_NAME, "ServiceNumero05:compte@exemple.org", 34);
    n = oath_cmd(&ctx, 0x02, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT_EQ(n, OATH_SW_NEEDS_TOUCH, "DELETE demande l'appui");
    n = oath_touch_commit(&ctx, true, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x90, 0x00), "supprime");
    TEST_ASSERT_EQ(ctx.pending_len, 0, "le differe est purge par DELETE");
    n = oath_cmd(&ctx, 0xA5, 0x00, 0x00, NULL, 0, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x6A, 0x82),
                "SEND REMAINING ne sert plus le compte supprime");

    /* --- PUT --- */
    sec_store_init();
    for (uint8_t i = 0; i < 12; i++) {
        char nom[40];
        snprintf(nom, sizeof(nom), "ServiceNumero%02u:compte@exemple.org", (unsigned)i);
        slot_totp(i, nom);
    }
    oath_select_ok(&ctx);
    n = oath_cmd(&ctx, 0xA1, 0x00, 0x00, NULL, 0, out, sizeof(out));
    TEST_ASSERT(ctx.pending_len > 0, "un reste est en attente");
    {
        uint8_t clef[2 + 20];
        clef[0] = OATH_ALGO_TOTP_SHA1; clef[1] = 6;
        memset(&clef[2], 0xAB, 20);
        l = tlv_at(d, 0, OATH_TAG_NAME, "Neuf:n", 6);
        l = tlv_at(d, l, OATH_TAG_KEY, clef, sizeof(clef));
    }
    n = oath_cmd(&ctx, 0x01, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x90, 0x00), "PUT accepte");
    TEST_ASSERT_EQ(ctx.pending_len, 0, "le differe est purge par PUT");

    /* --- RENAME --- */
    sec_store_init();
    for (uint8_t i = 0; i < 12; i++) {
        char nom[40];
        snprintf(nom, sizeof(nom), "ServiceNumero%02u:compte@exemple.org", (unsigned)i);
        slot_totp(i, nom);
    }
    oath_select_ok(&ctx);
    n = oath_cmd(&ctx, 0xA1, 0x00, 0x00, NULL, 0, out, sizeof(out));
    TEST_ASSERT(ctx.pending_len > 0, "un reste est en attente");
    l = tlv_at(d, 0, OATH_TAG_NAME, "ServiceNumero05:compte@exemple.org", 34);
    l = tlv_at(d, l, OATH_TAG_NAME, "Renomme:r", 9);
    n = oath_cmd(&ctx, 0x05, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x90, 0x00), "RENAME accepte");
    TEST_ASSERT_EQ(ctx.pending_len, 0, "le differe est purge par RENAME");

    /* --- REPLACE confirme --- */
    sec_store_init();
    for (uint8_t i = 0; i < 12; i++) {
        char nom[40];
        snprintf(nom, sizeof(nom), "ServiceNumero%02u:compte@exemple.org", (unsigned)i);
        slot_totp(i, nom);
    }
    oath_select_ok(&ctx);
    n = oath_cmd(&ctx, 0xA1, 0x00, 0x00, NULL, 0, out, sizeof(out));
    TEST_ASSERT(ctx.pending_len > 0, "un reste est en attente");
    l = put_body(d, "ServiceNumero05:compte@exemple.org", 34, 6, 0xEE, 20);
    n = oath_cmd(&ctx, 0x01, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT_EQ(n, OATH_SW_NEEDS_TOUCH, "REPLACE demande l'appui");
    n = oath_touch_commit(&ctx, true, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x90, 0x00), "remplace");
    TEST_ASSERT_EQ(ctx.pending_len, 0, "le differe est purge par le remplacement");
    n = oath_cmd(&ctx, 0xA5, 0x00, 0x00, NULL, 0, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x6A, 0x82),
                "SEND REMAINING ne sert plus l'etat d'avant le remplacement");

    /* --- RESET confirme --- */
    sec_store_init();
    for (uint8_t i = 0; i < 12; i++) {
        char nom[40];
        snprintf(nom, sizeof(nom), "ServiceNumero%02u:compte@exemple.org", (unsigned)i);
        slot_totp(i, nom);
    }
    oath_select_ok(&ctx);
    n = oath_cmd(&ctx, 0xA1, 0x00, 0x00, NULL, 0, out, sizeof(out));
    TEST_ASSERT(ctx.pending_len > 0, "un reste est en attente");
    n = oath_cmd(&ctx, 0x04, 0xDE, 0xAD, NULL, 0, out, sizeof(out));
    TEST_ASSERT_EQ(n, OATH_SW_NEEDS_TOUCH, "RESET demande l'appui");
    n = oath_touch_commit(&ctx, true, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x90, 0x00), "remis a neuf");
    TEST_ASSERT_EQ(ctx.pending_len, 0, "le differe est purge par RESET");
    n = oath_cmd(&ctx, 0xA5, 0x00, 0x00, NULL, 0, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x6A, 0x82),
                "SEND REMAINING ne sert plus les comptes effaces");

    /* --- SELECT --- */
    sec_store_init();
    for (uint8_t i = 0; i < 12; i++) {
        char nom[40];
        snprintf(nom, sizeof(nom), "ServiceNumero%02u:compte@exemple.org", (unsigned)i);
        slot_totp(i, nom);
    }
    oath_select_ok(&ctx);
    n = oath_cmd(&ctx, 0xA1, 0x00, 0x00, NULL, 0, out, sizeof(out));
    TEST_ASSERT(ctx.pending_len > 0, "un reste est en attente");
    n = oath_cmd(&ctx, 0xA4, 0x04, 0x00, k_aid, sizeof(k_aid), out, sizeof(out));
    TEST_ASSERT(n > 2, "re-selection");
    TEST_ASSERT_EQ(ctx.pending_len, 0, "le differe est purge par SELECT");
    n = oath_cmd(&ctx, 0xA5, 0x00, 0x00, NULL, 0, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x6A, 0x82),
                "une re-selection ne laisse pas reprendre la reponse d'avant");
}

/*
 * I3 — le coffre n'a que cr_hmac_sha1. Un compte provisionne en SHA-256 (que
 * ykman propose) serait accepte, persiste, et rendrait ETERNELLEMENT des codes
 * faux sans qu'aucune erreur ne le dise. Refus explicite plutot que mensonge
 * silencieux.
 */
static void test_put_n_accepte_que_totp_sha1(void)
{
    uint8_t out[32], d[64];
    const uint8_t refuses[] = { 0x22, 0x23, 0x2F, 0x20, 0x11, 0x31 };

    for (unsigned i = 0; i < sizeof(refuses); i++) {
        sec_store_init();
        oath_ctx_t ctx;
        oath_select_ok(&ctx);
        uint8_t clef[2 + 20];
        clef[0] = refuses[i]; clef[1] = 6;
        memset(&clef[2], 0xAB, 20);
        uint8_t l = tlv_at(d, 0, OATH_TAG_NAME, "A:b", 3);
        l = tlv_at(d, l, OATH_TAG_KEY, clef, sizeof(clef));
        uint16_t n = oath_cmd(&ctx, 0x01, 0x00, 0x00, d, l, out, sizeof(out));
        TEST_ASSERT(sw_is(out, n, 0x6A, 0x81), "6A81 : algorithme hors portee");
        TEST_ASSERT_EQ(sec_store_count(), 0, "rien n'a ete persiste");
    }

    /* Temoin positif : 0x21 (TOTP/SHA1) passe. Sans lui, « tout refuser »
     * satisferait la boucle ci-dessus. */
    sec_store_init();
    oath_ctx_t ctx;
    oath_select_ok(&ctx);
    uint8_t clef[2 + 20];
    clef[0] = OATH_ALGO_TOTP_SHA1; clef[1] = 6;
    memset(&clef[2], 0xAB, 20);
    uint8_t l = tlv_at(d, 0, OATH_TAG_NAME, "A:b", 3);
    l = tlv_at(d, l, OATH_TAG_KEY, clef, sizeof(clef));
    uint16_t n = oath_cmd(&ctx, 0x01, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x90, 0x00), "0x21 accepte");
    TEST_ASSERT_EQ(sec_store_count(), 1, "le compte est persiste");
    TEST_ASSERT_EQ(sec_store_type(0), OATH_ALGO_TOTP_SHA1, "le type persiste est 0x21");
}

/*
 * I4 — chaque borne testee LA OU elle agit, pas la ou une garde en aval rend
 * par hasard le meme mot d'etat.
 */

/* La malformation prime sur la capacite : magasin plein ET secret trop long
 * doit rendre 6A80 (donnee invalide), pas 6A84 (plus de place). Sans cet
 * ordre, la garde de longueur serait indistinguable de celle de sec_store. */
static void test_put_secret_trop_long_prime_sur_magasin_plein(void)
{
    sec_store_init();
    oath_ctx_t ctx;
    for (uint8_t i = 0; i < SEC_N_SLOTS; i++) {
        char nom[16];
        snprintf(nom, sizeof(nom), "S%02u:c", (unsigned)i);
        slot_totp(i, nom);
    }
    oath_select_ok(&ctx);

    uint8_t d[3 + 2 + 2 + 65], out[32];
    uint8_t clef[2 + 65];
    clef[0] = OATH_ALGO_TOTP_SHA1; clef[1] = 6;
    memset(&clef[2], 0xCD, 65);
    uint8_t l = tlv_at(d, 0, OATH_TAG_NAME, "ZZZ", 3);
    l = tlv_at(d, l, OATH_TAG_KEY, clef, sizeof(clef));
    uint16_t n = oath_cmd(&ctx, 0x01, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x6A, 0x80),
                "6A80 : la longueur se refuse avant de chercher un slot");
    TEST_ASSERT_EQ(sec_store_count(), SEC_N_SLOTS, "aucun compte ecrase");
}

/* Les chiffres se valident AVANT toute ecriture : les valider apres laisserait
 * un slot ecrit puis un refus, c'est-a-dire un compte a demi provisionne. */
static void test_put_chiffres_valides_avant_ecriture(void)
{
    const uint8_t mauvais[] = { 0, 1, 5, 7, 9, 10, 255 };
    for (unsigned i = 0; i < sizeof(mauvais); i++) {
        sec_store_init();
        oath_ctx_t ctx;
        oath_select_ok(&ctx);
        uint8_t d[64], out[32];
        uint8_t clef[2 + 20];
        clef[0] = OATH_ALGO_TOTP_SHA1; clef[1] = mauvais[i];
        memset(&clef[2], 0xAB, 20);
        uint8_t l = tlv_at(d, 0, OATH_TAG_NAME, "A:b", 3);
        l = tlv_at(d, l, OATH_TAG_KEY, clef, sizeof(clef));
        uint16_t n = oath_cmd(&ctx, 0x01, 0x00, 0x00, d, l, out, sizeof(out));
        TEST_ASSERT(sw_is(out, n, 0x6A, 0x80), "6A80 : chiffres hors {6,8}");
        TEST_ASSERT_EQ(sec_store_count(), 0,
                       "AUCUN slot ecrit — pas de compte a demi provisionne");
    }
    /* Huit chiffres, l'autre valeur legitime : elle doit passer. */
    sec_store_init();
    oath_ctx_t ctx;
    oath_select_ok(&ctx);
    uint8_t d[64], out[32];
    uint8_t clef[2 + 20];
    clef[0] = OATH_ALGO_TOTP_SHA1; clef[1] = 8;
    memset(&clef[2], 0xAB, 20);
    uint8_t l = tlv_at(d, 0, OATH_TAG_NAME, "A:b", 3);
    l = tlv_at(d, l, OATH_TAG_KEY, clef, sizeof(clef));
    uint16_t n = oath_cmd(&ctx, 0x01, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x90, 0x00), "huit chiffres acceptes");
    TEST_ASSERT_EQ(sec_store_digits(0), 8, "les chiffres sont persistes");
}

/* La correspondance de nom est EXACTE : un prefixe ne doit pas ouvrir le
 * compte qui le prolonge. */
static void test_nom_partiel_ne_correspond_pas(void)
{
    sec_store_init();
    oath_ctx_t ctx;
    slot_totp(0, "GitHub:mae@exemple.org");
    oath_select_ok(&ctx);
    uint8_t d[64], out[32];

    uint8_t l = tlv_at(d, 0, OATH_TAG_NAME, "GitHub", 6);
    uint16_t n = oath_cmd(&ctx, 0x02, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x6A, 0x82), "un prefixe n'est pas le compte");
    TEST_ASSERT_EQ(sec_store_count(), 1, "le compte survit");

    /* Un nom plus LONG que l'etiquette ne doit pas correspondre non plus. */
    l = tlv_at(d, 0, OATH_TAG_NAME, "GitHub:mae@exemple.orgX", 23);
    n = oath_cmd(&ctx, 0x02, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x6A, 0x82), "un sur-nom n'est pas le compte");

    /* Temoin : le nom exact, lui, correspond. */
    l = tlv_at(d, 0, OATH_TAG_NAME, "GitHub:mae@exemple.org", 22);
    n = oath_cmd(&ctx, 0x02, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT_EQ(n, OATH_SW_NEEDS_TOUCH, "le nom exact correspond");
}

/*
 * Un octet nul dans le nom serait tronque par le strncpy de sec_store : deux
 * comptes distincts cote hote deviendraient le meme cote clef, et le second
 * ecraserait le premier en silence.
 */
static void test_put_refuse_l_octet_nul_dans_le_nom(void)
{
    sec_store_init();
    oath_ctx_t ctx;
    oath_select_ok(&ctx);
    uint8_t d[64], out[32];
    uint8_t clef[2 + 20];
    clef[0] = OATH_ALGO_TOTP_SHA1; clef[1] = 6;
    memset(&clef[2], 0xAB, 20);

    uint8_t l = tlv_at(d, 0, OATH_TAG_NAME, "A\0B", 3);
    l = tlv_at(d, l, OATH_TAG_KEY, clef, sizeof(clef));
    uint16_t n = oath_cmd(&ctx, 0x01, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x6A, 0x80), "6A80 : octet nul dans le nom");
    TEST_ASSERT_EQ(sec_store_count(), 0,
                   "rien n'est ecrit — sinon l'etiquette serait « A » tout court");
}

/* CALCULATE ALL n'existe que sur P2=01 : deviner l'intention d'un autre P2
 * serait pire que la refuser. */
static void test_calculate_all_exige_p2_01(void)
{
    sec_store_init();
    oath_ctx_t ctx;
    slot_totp(0, "A:b");
    oath_select_ok(&ctx);
    uint8_t out[64];

    uint16_t n = oath_cmd(&ctx, 0xA4, 0x00, 0x00, NULL, 0, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x6A, 0x80), "6A80 : 0xA4 ni SELECT ni CALCULATE ALL");
    n = oath_cmd(&ctx, 0xA4, 0x00, 0x02, NULL, 0, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x6A, 0x80), "6A80 : P2=02 n'est pas CALCULATE ALL");
    /* Temoin : P2=01 repond bien. */
    n = oath_cmd(&ctx, 0xA4, 0x00, 0x01, NULL, 0, out, sizeof(out));
    TEST_ASSERT(n > 2, "P2=01 rend la liste");
}

/* SEND REMAINING sans reste en attente n'est pas « fin de transfert » : c'est
 * une reprise de rien, et 9000 ferait croire a l'hote a une reponse vide. */
static void test_send_remaining_sans_reste(void)
{
    sec_store_init();
    oath_ctx_t ctx;
    oath_select_ok(&ctx);
    uint8_t out[64];
    uint16_t n = oath_cmd(&ctx, 0xA5, 0x00, 0x00, NULL, 0, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x6A, 0x82), "6A82 : rien a reprendre");
    TEST_ASSERT(!(out[0] == 0x90 && out[1] == 0x00),
                "surtout pas 9000 : ce n'est pas une reponse vide");
}

/*
 * I5 — verifier la PRESENCE du 0x71 ne suffit pas : l'emettre a longueur nulle
 * laisserait tout vert, et _get_device_id() calculerait alors le meme
 * identifiant sur toutes les unites.
 */
static void test_select_contenu_de_la_version_et_du_sel(void)
{
    sec_store_init();
    oath_ctx_t ctx;
    oath_select_ok(&ctx);

    uint8_t out[64];
    /* Re-selection : oath_select_ok a deja arme le contexte avec un sel
     * reconnaissable, on relit la reponse pour l'inspecter. */
    uint16_t n = oath_cmd(&ctx, 0xA4, 0x04, 0x00, k_aid, sizeof(k_aid), out, sizeof(out));
    TEST_ASSERT(n > 2, "le SELECT rend des donnees");
    const uint16_t body = (uint16_t)(n - 2);

    const uint8_t *v = NULL; uint16_t vl = 0;
    TEST_ASSERT(oath_tlv_find(out, body, OATH_TAG_VERSION, &v, &vl), "0x79 present");
    TEST_ASSERT_EQ(vl, 3, "trois octets de version");
    TEST_ASSERT(vl == 3 && v[0] == 0x05 && v[1] == 0x07 && v[2] == 0x01,
                "version 5.7.1, pas trois octets quelconques");

    TEST_ASSERT(oath_tlv_find(out, body, OATH_TAG_NAME, &v, &vl), "0x71 present");
    TEST_ASSERT_EQ(vl, OATH_SALT_LEN, "le sel fait huit octets, pas zero");
    TEST_ASSERT(vl == OATH_SALT_LEN && memcmp(v, ctx.salt, OATH_SALT_LEN) == 0,
                "le sel rendu est celui du contexte, pas une constante");
}

/* Minor : n'importe quel AID armait l'applet. */
static void test_select_verifie_l_aid(void)
{
    sec_store_init();
    oath_ctx_t ctx; memset(&ctx, 0, sizeof(ctx));
    uint8_t out[64];
    const uint8_t autre[7] = { 0xA0, 0x00, 0x00, 0x06, 0x47, 0x2F, 0x00 };
    uint16_t n = oath_cmd(&ctx, 0xA4, 0x04, 0x00, autre, sizeof(autre), out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x6A, 0x82), "6A82 : cet AID n'est pas le notre");
    TEST_ASSERT(!ctx.selected, "un AID etranger n'arme pas l'applet");

    n = oath_cmd(&ctx, 0xA4, 0x04, 0x00, k_aid, sizeof(k_aid), out, sizeof(out));
    TEST_ASSERT(n > 2 && ctx.selected, "l'AID YKOATH, lui, arme l'applet");
}

/* Minor : la classe n'etait jamais examinee. */
static void test_cla_non_nulle_refusee(void)
{
    sec_store_init();
    oath_ctx_t ctx;
    slot_totp(0, "A:b");
    oath_select_ok(&ctx);
    uint8_t out[64];
    uint16_t n = oath_cmd_cla(&ctx, 0x80, 0xA1, 0x00, 0x00, NULL, 0, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x6E, 0x00), "6E00 : classe non supportee");
}

/* Minor : un doublon d'etiquette rendrait le second slot inatteignable — donc
 * indelebile, puisque toute commande passe par le nom. */
static void test_rename_refuse_un_doublon(void)
{
    sec_store_init();
    oath_ctx_t ctx;
    slot_totp(0, "A:b");
    slot_totp(1, "C:d");
    oath_select_ok(&ctx);
    uint8_t d[32], out[32];

    uint8_t l = tlv_at(d, 0, OATH_TAG_NAME, "A:b", 3);
    l = tlv_at(d, l, OATH_TAG_NAME, "C:d", 3);
    uint16_t n = oath_cmd(&ctx, 0x05, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x6A, 0x80), "6A80 : ce nom est deja pris");
    TEST_ASSERT(strcmp(sec_store_label(0), "A:b") == 0, "le premier compte est intact");
    TEST_ASSERT(strcmp(sec_store_label(1), "C:d") == 0, "le second compte est intact");
}


/*
 * Creer un compte ne detruit rien : douze appuis a la migration depuis Proton
 * seraient douze occasions d'apprendre a confirmer sans regarder. Un PUT sur un
 * nom inconnu passe donc directement.
 */
static void test_put_qui_cree_ne_demande_pas_l_appui(void)
{
    sec_store_init();
    oath_ctx_t ctx;
    oath_select_ok(&ctx);
    uint8_t d[96], out[32];

    uint8_t l = put_body(d, "A:b", 3, 6, 0xAA, 20);
    uint16_t n = oath_cmd(&ctx, 0x01, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x90, 0x00), "9000 : creation sans appui");
    TEST_ASSERT_EQ(ctx.touch_op, OATH_TOUCH_NONE, "aucun appui arme pour une creation");
    TEST_ASSERT_EQ(sec_store_count(), 1, "le compte est cree");

    uint8_t sec[SEC_SECRET_MAX]; uint8_t sl = 0;
    TEST_ASSERT(sec_store_get_secret(0, sec, &sl), "secret relisible");
    TEST_ASSERT_EQ(sl, 20, "longueur du secret");
    TEST_ASSERT_EQ(sec[0], 0xAA, "c'est bien le secret envoye");
}

/*
 * Un PUT sur un nom DEJA PRESENT remplace le slot : il detruit un secret aussi
 * surement qu'un DELETE, en moins visible. Il exige donc l'appui — avec une
 * operation distincte, parce que « REMPLACER » et « EFFACER » ne se refusent
 * pas pour les memes raisons.
 *
 * La propriete qui compte n'est pas le mot d'etat : c'est qu'un remplacement
 * NON confirme laisse l'ancien secret intact. Un test qui ne verifierait que
 * le mot d'etat raterait exactement ca.
 */
static void test_put_qui_ecrase_exige_l_appui(void)
{
    sec_store_init();
    oath_ctx_t ctx;
    uint8_t d[96], out[32], sec[SEC_SECRET_MAX], sl = 0;

    /* Compte existant : secret 0xAA sur 20 octets, six chiffres. */
    const uint8_t ancien[20] = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
                                 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
                                 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA };
    TEST_ASSERT(sec_store_set_slot(0, OATH_ALGO_TOTP_SHA1, "A:b", ancien, 20),
                "compte existant provisionne");
    TEST_ASSERT(sec_store_set_digits(0, 6), "six chiffres au depart");
    oath_select_ok(&ctx);

    /* Meme nom, autre secret, autre nombre de chiffres. */
    uint8_t l = put_body(d, "A:b", 3, 8, 0xBB, 32);
    uint16_t n = oath_cmd(&ctx, 0x01, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT_EQ(n, OATH_SW_NEEDS_TOUCH, "ecraser demande l'appui");
    TEST_ASSERT_EQ(ctx.touch_op, OATH_TOUCH_REPLACE, "operation REPLACE");
    TEST_ASSERT(ctx.touch_op != OATH_TOUCH_DELETE,
                "distincte d'EFFACER : l'ecran ne dit pas la meme chose");
    TEST_ASSERT_EQ(ctx.touch_slot, 0, "le slot vise est retenu");
    TEST_ASSERT_EQ(ctx.touch_count, 1, "un compte concerne");

    /* Avant tout appui : rien n'a bouge. */
    TEST_ASSERT(sec_store_get_secret(0, sec, &sl), "secret relisible");
    TEST_ASSERT_EQ(sl, 20, "l'ancienne longueur tient");
    TEST_ASSERT_EQ(sec[0], 0xAA, "l'ancien secret tient");
    TEST_ASSERT_EQ(sec_store_digits(0), 6, "les anciens chiffres tiennent");

    /* Appui REFUSE : l'ancien secret doit survivre en entier. */
    n = oath_touch_commit(&ctx, false, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x69, 0x85), "6985 : remplacement refuse");
    TEST_ASSERT_EQ(sec_store_count(), 1, "toujours un seul compte");
    TEST_ASSERT(sec_store_get_secret(0, sec, &sl), "secret relisible");
    TEST_ASSERT_EQ(sl, 20, "longueur inchangee apres refus");
    for (unsigned i = 0; i < 20; i++)
        TEST_ASSERT_EQ(sec[i], 0xAA, "octet de l'ancien secret intact");
    TEST_ASSERT_EQ(sec_store_digits(0), 6, "chiffres inchanges apres refus");
    TEST_ASSERT(strcmp(sec_store_label(0), "A:b") == 0, "etiquette inchangee");
    TEST_ASSERT_EQ(ctx.touch_op, OATH_TOUCH_NONE, "la demande est retiree");
    /* Le secret refuse ne doit pas trainer en RAM : il attendait une
     * confirmation qui n'est pas venue. */
    TEST_ASSERT_EQ(ctx.touch_put_secret_len, 0, "longueur du secret en attente remise a zero");
    TEST_ASSERT_EQ(ctx.touch_count, 0, "aucun compte annonce apres le refus");
    {
        bool reste = false;
        for (unsigned i = 0; i < SEC_SECRET_MAX; i++)
            if (ctx.touch_put_secret[i] != 0) reste = true;
        TEST_ASSERT(!reste, "le secret refuse est efface du contexte");
    }

    /* Une confirmation qui ne suit plus rien ne doit pas rejouer le
     * remplacement abandonne. */
    n = oath_touch_commit(&ctx, true, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x69, 0x85), "6985 : rien en attente");
    TEST_ASSERT(sec_store_get_secret(0, sec, &sl) && sec[0] == 0xAA,
                "l'ancien secret n'a pas ete remplace apres coup");

    /* Appui ACCORDE : le remplacement prend effet, en entier. */
    l = put_body(d, "A:b", 3, 8, 0xBB, 32);
    n = oath_cmd(&ctx, 0x01, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT_EQ(n, OATH_SW_NEEDS_TOUCH, "l'appui est redemande");
    n = oath_touch_commit(&ctx, true, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x90, 0x00), "9000 : remplace apres appui");
    TEST_ASSERT_EQ(sec_store_count(), 1, "toujours un seul compte, pas deux");
    TEST_ASSERT(sec_store_get_secret(0, sec, &sl), "secret relisible");
    TEST_ASSERT_EQ(sl, 32, "la nouvelle longueur a pris");
    for (unsigned i = 0; i < 32; i++)
        TEST_ASSERT_EQ(sec[i], 0xBB, "octet du nouveau secret");
    TEST_ASSERT_EQ(sec_store_digits(0), 8, "les nouveaux chiffres ont pris");
    TEST_ASSERT(strcmp(sec_store_label(0), "A:b") == 0, "l'etiquette est conservee");
    /* La purge du differe n'est PAS verifiable ici : un seul compte tient dans
     * une tranche, donc pending_len vaut 0 avant comme apres et l'assertion
     * serait creuse. Elle est couverte, LIST emis a l'appui, dans
     * test_pending_purge_par_chaque_mutation. */
}

/*
 * Un seul appui detruit douze secrets sur un RESET : l'ecran doit pouvoir dire
 * COMBIEN. Le contexte le porte, pour que la tache 5 n'ait pas a reanalyser le
 * magasin — deux comptages du meme etat, deux occasions de diverger.
 */
static void test_le_contexte_annonce_combien_de_comptes(void)
{
    sec_store_init();
    oath_ctx_t ctx;
    const uint8_t key[20] = { 0 };
    /* Un slot CR-HMAC parmi eux : il ne part pas au RESET, donc il ne doit pas
     * etre compte non plus. */
    TEST_ASSERT(sec_store_set_slot(0, SEC_SLOT_HMAC_SHA1, "keepassxc", key, 20),
                "slot CR-HMAC");
    for (uint8_t i = 1; i <= 5; i++) {
        char nom[16];
        snprintf(nom, sizeof(nom), "S%02u:c", (unsigned)i);
        slot_totp(i, nom);
    }
    oath_select_ok(&ctx);
    uint8_t out[32], d[32];

    uint16_t n = oath_cmd(&ctx, 0x04, 0xDE, 0xAD, NULL, 0, out, sizeof(out));
    TEST_ASSERT_EQ(n, OATH_SW_NEEDS_TOUCH, "RESET demande l'appui");
    TEST_ASSERT_EQ(ctx.touch_count, 5,
                   "cinq comptes OATH partiront, pas six : le CR-HMAC reste");

    /* Et le compte annonce est bien celui qui part. */
    n = oath_touch_commit(&ctx, true, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x90, 0x00), "RESET confirme");
    TEST_ASSERT_EQ(sec_store_count(), 1, "il ne reste que le slot CR-HMAC");

    /* DELETE n'en detruit qu'un : le champ doit suivre l'operation, pas rester
     * fige sur la derniere valeur. */
    sec_store_init();
    slot_totp(0, "A:b");
    slot_totp(1, "C:d");
    oath_select_ok(&ctx);
    uint8_t l = tlv_at(d, 0, OATH_TAG_NAME, "A:b", 3);
    n = oath_cmd(&ctx, 0x02, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT_EQ(n, OATH_SW_NEEDS_TOUCH, "DELETE demande l'appui");
    TEST_ASSERT_EQ(ctx.touch_count, 1, "un seul compte concerne");
}

/*
 * LE defaut que tout le mecanisme d'appui existe pour empecher : l'ecran
 * affiche « EFFACER ServiceNumero05 », la proprietaire appuie, et c'est un
 * autre compte qui disparait. Deux mutations passaient au vert —
 * `sec_store_clear_slot(0)` et `sec_store_set_slot(0, ...)` a la place de
 * `ctx->touch_slot`.
 *
 * On vise donc un slot != 0 et on verifie que le VOISIN survit intact, secret
 * compris : compter les comptes restants ne dirait pas lequel est parti.
 */
static void test_l_appui_confirme_agit_sur_le_bon_compte(void)
{
    sec_store_init();
    oath_ctx_t ctx;
    for (uint8_t i = 0; i < 5; i++) {
        char nom[16];
        snprintf(nom, sizeof(nom), "S%02u:c", (unsigned)i);
        slot_totp_secret(i, nom, (uint8_t)(0x10 + i));
    }
    oath_select_ok(&ctx);
    uint8_t d[96], out[32];

    /* --- DELETE confirme sur le slot 3 --- */
    uint8_t l = tlv_at(d, 0, OATH_TAG_NAME, "S03:c", 5);
    uint16_t n = oath_cmd(&ctx, 0x02, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT_EQ(n, OATH_SW_NEEDS_TOUCH, "DELETE demande l'appui");
    TEST_ASSERT_EQ(ctx.touch_slot, 3, "le slot vise est le 3");
    n = oath_touch_commit(&ctx, true, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x90, 0x00), "9000 : supprime");

    TEST_ASSERT_EQ(sec_store_type(3), SEC_SLOT_EMPTY, "c'est bien le slot 3 qui part");
    TEST_ASSERT(slot_intact(0, "S00:c", 0x10), "le slot 0 est intact, secret compris");
    TEST_ASSERT(slot_intact(1, "S01:c", 0x11), "le slot 1 est intact, secret compris");
    TEST_ASSERT(slot_intact(2, "S02:c", 0x12), "le slot 2 est intact, secret compris");
    TEST_ASSERT(slot_intact(4, "S04:c", 0x14), "le slot 4 est intact, secret compris");

    /* --- REPLACE confirme sur le slot 2 --- */
    l = put_body(d, "S02:c", 5, 8, 0xEE, 24);
    n = oath_cmd(&ctx, 0x01, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT_EQ(n, OATH_SW_NEEDS_TOUCH, "REPLACE demande l'appui");
    TEST_ASSERT_EQ(ctx.touch_slot, 2, "le slot vise est le 2");
    n = oath_touch_commit(&ctx, true, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x90, 0x00), "9000 : remplace");

    uint8_t sec[SEC_SECRET_MAX]; uint8_t sl = 0;
    TEST_ASSERT(sec_store_get_secret(2, sec, &sl), "secret du slot 2 relisible");
    TEST_ASSERT_EQ(sl, 24, "c'est le slot 2 qui a recu le nouveau secret");
    TEST_ASSERT_EQ(sec[0], 0xEE, "nouveau secret au slot 2");
    TEST_ASSERT(slot_intact(0, "S00:c", 0x10),
                "le slot 0 n'a PAS ete ecrase par le remplacement");
    TEST_ASSERT(slot_intact(1, "S01:c", 0x11), "le slot 1 est intact");
    TEST_ASSERT(slot_intact(4, "S04:c", 0x14), "le slot 4 est intact");
}

/*
 * oath_do_put a sa PROPRE boucle de recherche de slot libre — elle ne passe
 * pas par oath_find_slot. Un slot libre est un slot VIDE, jamais « un slot qui
 * n'est pas a nous » : confondre les deux ferait ecrire le compte OATH par
 * dessus le secret CR-HMAC de KeePassXC.
 */
static void test_put_ne_prend_pas_le_slot_du_mode_otp(void)
{
    sec_store_init();
    oath_ctx_t ctx;
    uint8_t crh[20];
    memset(crh, 0x5A, sizeof(crh));
    TEST_ASSERT(sec_store_set_slot(0, SEC_SLOT_HMAC_SHA1, "keepassxc", crh, sizeof(crh)),
                "slot CR-HMAC en slot 0");
    oath_select_ok(&ctx);
    uint8_t d[96], out[32];

    uint8_t l = put_body(d, "Neuf:n", 6, 6, 0xCC, 20);
    uint16_t n = oath_cmd(&ctx, 0x01, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x90, 0x00), "9000 : le compte est cree");
    TEST_ASSERT_EQ(ctx.touch_op, OATH_TOUCH_NONE, "creation, donc pas d'appui");

    TEST_ASSERT_EQ(sec_store_type(0), SEC_SLOT_HMAC_SHA1,
                   "le slot 0 est reste CR-HMAC");
    uint8_t sec[SEC_SECRET_MAX]; uint8_t sl = 0;
    TEST_ASSERT(sec_store_get_secret(0, sec, &sl) && sl == 20 && sec[0] == 0x5A,
                "le secret KeePassXC est intact");
    TEST_ASSERT(sec_store_label(1) != NULL && strcmp(sec_store_label(1), "Neuf:n") == 0,
                "le compte OATH a pris le premier slot VIDE, le 1");
}

/*
 * PUT et RENAME ne passent pas par oath_find_slot pour choisir ou ecrire : ils
 * doivent donc verifier eux-memes qu'une etiquette n'est pas deja portee par
 * un slot d'un AUTRE mode. Sans cela, deux entrees « keepassxc » coexistent
 * dans le magasin — et c'est exactement le doublon que ce controle ferme.
 */
static void test_put_et_rename_ne_reprennent_pas_une_etiquette_du_mode_otp(void)
{
    sec_store_init();
    oath_ctx_t ctx;
    uint8_t crh[20];
    memset(crh, 0x5A, sizeof(crh));
    TEST_ASSERT(sec_store_set_slot(0, SEC_SLOT_HMAC_SHA1, "keepassxc", crh, sizeof(crh)),
                "slot CR-HMAC");
    slot_totp_secret(1, "A:b", 0x11);
    oath_select_ok(&ctx);
    uint8_t d[96], out[32];

    /* PUT « keepassxc » : ni remplacement du slot CR-HMAC, ni doublon. */
    uint8_t l = put_body(d, "keepassxc", 9, 6, 0xCC, 20);
    uint16_t n = oath_cmd(&ctx, 0x01, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x6A, 0x80), "6A80 : cette etiquette n'est pas libre");
    TEST_ASSERT_EQ(ctx.touch_op, OATH_TOUCH_NONE, "aucun appui arme");
    TEST_ASSERT_EQ(sec_store_type(0), SEC_SLOT_HMAC_SHA1, "le slot CR-HMAC est intact");
    uint8_t sec[SEC_SECRET_MAX]; uint8_t sl = 0;
    TEST_ASSERT(sec_store_get_secret(0, sec, &sl) && sec[0] == 0x5A,
                "le secret KeePassXC est intact");
    TEST_ASSERT_EQ(sec_store_count(), 2, "aucun compte n'a ete ajoute");

    /* RENAME « A:b » -> « keepassxc » : meme refus. */
    l = tlv_at(d, 0, OATH_TAG_NAME, "A:b", 3);
    l = tlv_at(d, l, OATH_TAG_NAME, "keepassxc", 9);
    n = oath_cmd(&ctx, 0x05, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x6A, 0x80), "6A80 : ce nom est deja porte");
    TEST_ASSERT(slot_intact(1, "A:b", 0x11), "le compte OATH n'a pas ete renomme");
    TEST_ASSERT(sec_store_label(0) != NULL && strcmp(sec_store_label(0), "keepassxc") == 0,
                "une seule entree porte « keepassxc »");

    /* Le controle porte sur l'etiquette ENTIERE : « keepass », strict prefixe
     * d'une etiquette existante, reste un nom libre. Une comparaison par
     * prefixe interdirait de creer des comptes parfaitement legitimes. */
    l = put_body(d, "keepass", 7, 6, 0xCC, 20);
    n = oath_cmd(&ctx, 0x01, 0x00, 0x00, d, l, out, sizeof(out));
    TEST_ASSERT(sw_is(out, n, 0x90, 0x00), "9000 : un prefixe est un autre nom");
    TEST_ASSERT_EQ(sec_store_count(), 3, "le compte a bien ete cree");
}

/* Le contrat le dit, rien ne le verifiait : achever un CALCULATE n'est pas le
 * travail de oath_touch_commit — il rend 0 et NE consomme PAS la demande. */
static void test_touch_commit_rend_zero_sur_calculate(void)
{
    sec_store_init();
    oath_ctx_t ctx;
    slot_totp(2, "A:b");
    oath_select_ok(&ctx);
    const uint8_t defi[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t d[32], out[32];
    uint8_t l = tlv_at(d, 0, OATH_TAG_NAME, "A:b", 3);
    l = tlv_at(d, l, OATH_TAG_CHALLENGE, defi, 8);
    uint16_t n = oath_cmd(&ctx, 0xA2, 0x00, 0x01, d, l, out, sizeof(out));
    TEST_ASSERT_EQ(n, OATH_SW_NEEDS_TOUCH, "l'appui est demande");

    memset(out, 0x5A, sizeof(out));
    TEST_ASSERT_EQ(oath_touch_commit(&ctx, true, out, sizeof(out)), 0,
                   "0 : le calcul reste a l'appelant, qui a le HMAC");
    TEST_ASSERT(out[0] == 0x5A, "aucun mot d'etat ecrit");
    TEST_ASSERT_EQ(ctx.touch_op, OATH_TOUCH_CALCULATE,
                   "la demande n'est pas consommee : l'appelant en a besoin");
    TEST_ASSERT_EQ(ctx.touch_slot, 2, "le slot vise reste disponible");
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
    TEST_RUN(test_rename_lit_le_second_nom);
    TEST_RUN(test_calculate_all_ne_rend_aucun_code);
    TEST_RUN(test_capacite_de_sortie_respectee);
    TEST_RUN(test_slots_cr_hmac_invisibles_a_oath);
    TEST_RUN(test_reset_exige_de_ad);
    TEST_RUN(test_delete_et_reset_exigent_l_appui);
    TEST_RUN(test_calculate_renseigne_le_contexte_d_appui);
    TEST_RUN(test_pending_purge_par_chaque_mutation);
    TEST_RUN(test_put_n_accepte_que_totp_sha1);
    TEST_RUN(test_put_secret_trop_long_prime_sur_magasin_plein);
    TEST_RUN(test_put_chiffres_valides_avant_ecriture);
    TEST_RUN(test_nom_partiel_ne_correspond_pas);
    TEST_RUN(test_put_refuse_l_octet_nul_dans_le_nom);
    TEST_RUN(test_calculate_all_exige_p2_01);
    TEST_RUN(test_send_remaining_sans_reste);
    TEST_RUN(test_select_contenu_de_la_version_et_du_sel);
    TEST_RUN(test_select_verifie_l_aid);
    TEST_RUN(test_cla_non_nulle_refusee);
    TEST_RUN(test_rename_refuse_un_doublon);
    TEST_RUN(test_put_qui_cree_ne_demande_pas_l_appui);
    TEST_RUN(test_put_qui_ecrase_exige_l_appui);
    TEST_RUN(test_le_contexte_annonce_combien_de_comptes);
    TEST_RUN(test_l_appui_confirme_agit_sur_le_bon_compte);
    TEST_RUN(test_put_ne_prend_pas_le_slot_du_mode_otp);
    TEST_RUN(test_put_et_rename_ne_reprennent_pas_une_etiquette_du_mode_otp);
    TEST_RUN(test_touch_commit_rend_zero_sur_calculate);
}
