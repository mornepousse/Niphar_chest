/* Tests de l'heure murale du coffre.
 *
 * Ecrits avant l'implementation (norme TDD du CLAUDE.md). Le coffre n'a AUCUNE
 * horloge : ni RTC, ni pile. Ce module ne fait donc qu'une chose — retenir une
 * heure POSEE et l'avancer sur le temps monotone — et c'est precisement le
 * genre de calcul qui peut etre faux sans etre visible : un code TOTP faux est
 * un code plausible.
 */
#include "test_framework.h"

#include "sec_time.h"

#include <string.h>

/* Le temps monotone est passe en parametre, jamais lu depuis esp_timer : c'est
 * ce qui rend ce module testable sur l'hote, et c'est la meme convention que
 * sec_confirm. */

static void test_invalide_au_depart(void)
{
    sec_time_reset();
    TEST_ASSERT(!sec_time_is_valid(), "aucune heure au demarrage");

    uint64_t u = 12345u;
    TEST_ASSERT(!sec_time_now(1000u, &u), "aucune heure a rendre");
    TEST_ASSERT_EQ(u, 12345u, "la sortie n'est pas touchee quand il n'y a rien");
}

/*
 * LE BIT NE SE LEVE JAMAIS SUR UNE HEURE PAR DEFAUT. Ni zero, ni l'epoque de
 * compilation, ni « probablement apres 2020 ». Une heure devinee produirait des
 * codes faux PRESENTES COMME JUSTES — pire que pas de code du tout, puisque le
 * clavier peut dire « NO TIME » et que la proprietaire sait alors quoi faire.
 */
static void test_zero_n_est_pas_une_heure(void)
{
    sec_time_reset();
    TEST_ASSERT(!sec_time_set(0u, 1000u), "zero refuse");
    TEST_ASSERT(!sec_time_is_valid(), "et rien n'est retenu");

    /* Une date anterieure a l'existence de TOTP ne peut pas etre une vraie
     * heure murale : c'est un defaut d'initialisation qui se deguise. */
    TEST_ASSERT(!sec_time_set(1u, 1000u), "epoque unix refusee");
    TEST_ASSERT(!sec_time_is_valid(), "toujours rien");
    TEST_ASSERT(!sec_time_set(SEC_TIME_MIN_PLAUSIBLE - 1u, 1000u),
                "juste sous le plancher : refusee");
    TEST_ASSERT(sec_time_set(SEC_TIME_MIN_PLAUSIBLE, 1000u), "le plancher est accepte");
    TEST_ASSERT(sec_time_is_valid(), "et il compte");
}

static void test_avance_sur_le_temps_monotone(void)
{
    sec_time_reset();
    TEST_ASSERT(sec_time_set(1790000000u, 5000u), "heure posee");

    uint64_t u = 0;
    TEST_ASSERT(sec_time_now(5000u, &u), "lisible tout de suite");
    TEST_ASSERT_EQ(u, 1790000000u, "au moment ou elle est posee, elle vaut ce qu'on a mis");

    TEST_ASSERT(sec_time_now(5000u + 30000u, &u), "trente secondes plus tard");
    TEST_ASSERT_EQ(u, 1790000030u, "trente secondes de plus");

    TEST_ASSERT(sec_time_now(5000u + 999u, &u), "moins d'une seconde");
    TEST_ASSERT_EQ(u, 1790000000u, "pas de seconde entamee comptee en trop");

    TEST_ASSERT(sec_time_now(5000u + 1000u, &u), "une seconde pile");
    TEST_ASSERT_EQ(u, 1790000001u, "une seconde");
}

/*
 * Le temps monotone en millisecondes reboucle a ~49,7 jours. Le coffre
 * n'existe que branche, donc il ne devrait jamais y arriver — « ne devrait
 * jamais » n'est pas une garantie, et une soustraction naive rendrait une heure
 * de quarante-neuf jours dans le passe sans que rien ne le dise.
 */
static void test_survit_au_rebouclage_des_millisecondes(void)
{
    sec_time_reset();
    const uint32_t avant = 0xFFFFF000u;
    TEST_ASSERT(sec_time_set(1790000000u, avant), "heure posee juste avant le rebouclage");

    uint64_t u = 0;
    /* 0x1000 ms apres 0xFFFFF000 : la valeur reboucle a 0x00000000. */
    TEST_ASSERT(sec_time_now(avant + 0x1000u, &u), "lisible apres le rebouclage");
    TEST_ASSERT_EQ(u, 1790000000u + 4u, "4096 ms = quatre secondes, pas quarante-neuf jours");
}

/* Un second set-time remplace le premier : l'hote peut corriger sa propre
 * derive sans qu'on ait a redemarrer le coffre. */
static void test_une_seconde_pose_remplace(void)
{
    sec_time_reset();
    TEST_ASSERT(sec_time_set(1790000000u, 1000u), "premiere pose");
    TEST_ASSERT(sec_time_set(1790005000u, 2000u), "seconde pose");

    uint64_t u = 0;
    TEST_ASSERT(sec_time_now(2000u, &u), "lisible");
    TEST_ASSERT_EQ(u, 1790005000u, "c'est la DERNIERE pose qui compte");
}

/* La fenetre TOTP : compteur et secondes restantes. Calcul pur, et c'est celui
 * qui rend un code juste ou faux. */
static void test_fenetre_totp(void)
{
    TEST_ASSERT_EQ(SEC_TIME_TOTP_STEP, 30u, "pas de trente secondes");

    TEST_ASSERT_EQ(sec_time_totp_counter(0u), 0u, "epoque");
    TEST_ASSERT_EQ(sec_time_totp_counter(29u), 0u, "encore la meme fenetre a 29 s");
    TEST_ASSERT_EQ(sec_time_totp_counter(30u), 1u, "fenetre suivante a 30 s");
    TEST_ASSERT_EQ(sec_time_totp_counter(59u), 1u, "toujours la meme a 59 s");

    /* Vecteur RFC 6238 : 59 s -> compteur 1, 1111111109 -> 0x23523EC. */
    TEST_ASSERT_EQ(sec_time_totp_counter(1111111109u), 0x23523ECu,
                   "vecteur RFC 6238");

    TEST_ASSERT_EQ(sec_time_window_remaining(0u), 30u, "tout le temps au debut");
    TEST_ASSERT_EQ(sec_time_window_remaining(1u), 29u, "une seconde ecoulee");
    TEST_ASSERT_EQ(sec_time_window_remaining(29u), 1u, "derniere seconde");
    TEST_ASSERT_EQ(sec_time_window_remaining(30u), 30u, "fenetre suivante, tout le temps");

    /* Jamais zero : un decompte a zero afficherait un code deja perime comme
     * s'il restait a l'utiliser. */
    for (uint64_t t = 0; t < 120u; t++) {
        const uint8_t r = sec_time_window_remaining(t);
        TEST_ASSERT(r >= 1u && r <= SEC_TIME_TOTP_STEP, "decompte toujours dans 1..30");
    }
}

void test_sec_time(void)
{
    TEST_SUITE("sec_time");
    TEST_RUN(test_invalide_au_depart);
    TEST_RUN(test_zero_n_est_pas_une_heure);
    TEST_RUN(test_avance_sur_le_temps_monotone);
    TEST_RUN(test_survit_au_rebouclage_des_millisecondes);
    TEST_RUN(test_une_seconde_pose_remplace);
    TEST_RUN(test_fenetre_totp);
}
