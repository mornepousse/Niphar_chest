/*
 * Décision de ZLP du pipe CCID — la logique pure extraite de ccid_drv_xfer().
 *
 * Ces tests existent à cause du BLOQUANT 2 de la revue finale de branche : le
 * seuil était codé en dur à 64 alors que le coffre négocie la haute vitesse,
 * où le MPS d'un endpoint bulk vaut 512 (mode_pgp.c:74). Une réponse de 64,
 * 128, 192 ou 256 octets est alors un paquet COURT, mais l'ancien test la
 * prenait pour un multiple exact du MPS : le firmware mettait une ZLP en file
 * et sortait sans réarmer OUT ni remettre s_busy — pipe CCID figé jusqu'à la
 * bascule de mode suivante. L'hôte choisit la longueur (10 octets d'en-tête
 * CCID + la réponse APDU) en écrivant puis relisant un DO OpenPGP
 * inscriptible : déni de service déclenchable depuis l'hôte.
 *
 * Le premier test ci-dessous est exactement ce cas.
 */
#include "test_framework.h"

#include "ccid_zlp.h"

/* Le MPS haute vitesse d'un endpoint bulk vaut 512, et lui seul (USB 2.0
 * tableau 5-5). En pleine vitesse le coffre déclare 64. */
#define MPS_HS 512u
#define MPS_FS  64u

/* LA régression. Sur un endpoint à 512, 64 octets forment un paquet court : le
 * transfert se termine de lui-même, aucune ZLP n'est nécessaire — et en
 * envoyer une figeait le pipe. */
static void test_short_packet_on_hs_needs_no_zlp(void)
{
    TEST_ASSERT(!ccid_needs_zlp(64u,  MPS_HS), "64 o sur un MPS de 512 : paquet court");
    TEST_ASSERT(!ccid_needs_zlp(128u, MPS_HS), "128 o sur un MPS de 512 : paquet court");
    TEST_ASSERT(!ccid_needs_zlp(192u, MPS_HS), "192 o sur un MPS de 512 : paquet court");
    TEST_ASSERT(!ccid_needs_zlp(256u, MPS_HS), "256 o sur un MPS de 512 : paquet court");
    TEST_ASSERT(!ccid_needs_zlp(320u, MPS_HS), "320 o sur un MPS de 512 : paquet court");
    TEST_ASSERT(!ccid_needs_zlp(448u, MPS_HS), "448 o sur un MPS de 512 : paquet court");
}

/* Les longueurs totales réellement atteignables par l'hôte : 10 octets
 * d'en-tête CCID + une réponse APDU de 54/118/182/246 octets, que les DO
 * OpenPGP inscriptibles (URL, login data, DO privés) permettent de viser. */
static void test_host_reachable_lengths_need_no_zlp(void)
{
    TEST_ASSERT(!ccid_needs_zlp(10u + 54u,  MPS_HS), "réponse APDU de 54 o : pas de ZLP");
    TEST_ASSERT(!ccid_needs_zlp(10u + 118u, MPS_HS), "réponse APDU de 118 o : pas de ZLP");
    TEST_ASSERT(!ccid_needs_zlp(10u + 182u, MPS_HS), "réponse APDU de 182 o : pas de ZLP");
    TEST_ASSERT(!ccid_needs_zlp(10u + 246u, MPS_HS), "réponse APDU de 246 o : pas de ZLP");
}

/* Un multiple EXACT du MPS ne porte pas de marqueur de fin implicite (USB 2.0
 * §5.8.3) : sans ZLP l'hôte continue d'attendre. C'est le cas pour lequel la
 * ZLP existe, et le correctif ne doit pas le faire disparaître. */
static void test_exact_multiple_needs_zlp(void)
{
    TEST_ASSERT(ccid_needs_zlp(MPS_HS, MPS_HS),      "512 o sur un MPS de 512 : ZLP requise");
    TEST_ASSERT(ccid_needs_zlp(2u * MPS_HS, MPS_HS), "1024 o sur un MPS de 512 : ZLP requise");
    TEST_ASSERT(ccid_needs_zlp(MPS_FS, MPS_FS),      "64 o sur un MPS de 64 : ZLP requise");
    TEST_ASSERT(ccid_needs_zlp(2u * MPS_FS, MPS_FS), "128 o sur un MPS de 64 : ZLP requise");
    TEST_ASSERT(ccid_needs_zlp(5u * MPS_FS, MPS_FS), "320 o sur un MPS de 64 : ZLP requise");
}

/* Pleine vitesse : le comportement d'origine, qui n'était juste que par
 * accident (le dongle KeSp n'énumère qu'à cette vitesse). Il doit le rester. */
static void test_full_speed_unchanged(void)
{
    TEST_ASSERT(!ccid_needs_zlp(63u, MPS_FS),  "63 o sur un MPS de 64 : paquet court");
    TEST_ASSERT(!ccid_needs_zlp(65u, MPS_FS),  "65 o sur un MPS de 64 : paquet court");
    TEST_ASSERT(!ccid_needs_zlp(281u, MPS_FS), "10 + 271 o (message CCID max) : paquet court");
}

/* xferred == 0, c'est la ZLP elle-même qui vient d'être acquittée : en
 * enchaîner une seconde bouclerait indéfiniment sur l'endpoint IN. */
static void test_zero_length_completion_never_chains(void)
{
    TEST_ASSERT(!ccid_needs_zlp(0u, MPS_HS), "ZLP acquittée en HS : pas de seconde ZLP");
    TEST_ASSERT(!ccid_needs_zlp(0u, MPS_FS), "ZLP acquittée en FS : pas de seconde ZLP");
    TEST_ASSERT(!ccid_needs_zlp(0u, 0u),     "ZLP acquittée, MPS inconnu : rien non plus");
}

/* MPS inconnu (endpoint pas encore ouvert). Des deux erreurs possibles, sauter
 * une ZLP fait au pire attendre l'hôte jusqu'à son propre timeout ; omettre le
 * réarmement de OUT fige le pipe pour de bon. Et un modulo par zéro serait un
 * comportement indéfini. */
static void test_unknown_mps_never_sends(void)
{
    TEST_ASSERT(!ccid_needs_zlp(1u, 0u),          "MPS inconnu : jamais de ZLP");
    TEST_ASSERT(!ccid_needs_zlp(512u, 0u),        "MPS inconnu : jamais de ZLP");
    TEST_ASSERT(!ccid_needs_zlp(0xFFFFFFFFu, 0u), "MPS inconnu : pas de modulo par zéro");
}

void test_ccid_zlp(void)
{
    TEST_SUITE("ccid_zlp");
    TEST_RUN(test_short_packet_on_hs_needs_no_zlp);
    TEST_RUN(test_host_reachable_lengths_need_no_zlp);
    TEST_RUN(test_exact_multiple_needs_zlp);
    TEST_RUN(test_full_speed_unchanged);
    TEST_RUN(test_zero_length_completion_never_chains);
    TEST_RUN(test_unknown_mps_never_sends);
}
