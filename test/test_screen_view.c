/* L'ecran est la SEULE chose qui dit POUR QUOI on appuie. Un libelle faux ou
 * partage entre deux operations est pire qu'une absence d'ecran : il fait
 * confirmer en croyant savoir. */
#include "test_framework.h"
#include <string.h>

#include "usb/usb_mode.h"
#include "sec_confirm.h"
#include "hmi/led_state.h"
#include "hmi/screen_view.h"

/* Totalite : aucun etat ne laisse l'ecran indefini, meme pour un mode aberrant.
 *
 * NOTE — ce test affirme SCREEN_IDLE et non « une des quatre valeurs de
 * l'enum ». La seconde formulation serait TAUTOLOGIQUE : l'enum n'a que ces
 * quatre valeurs, donc aucune implementation, meme absurde, ne la ferait
 * rougir. Avec ce jeu d'entrees (rien d'arme, aucun evenement), la seule
 * reponse juste est l'ecran de repos, et c'est cela qu'il faut exiger. */
static void test_every_state_has_a_screen(void)
{
    for (int m = 0; m <= USB_MODE_COUNT; m++) {
        screen_view_t v = screen_view_of((usb_mode_t)m, false, SEC_OP_UNKNOWN,
                                         LED_EVENT_NONE);
        TEST_ASSERT_EQ(v.kind, SCREEN_IDLE,
                       "rien d'arme et aucun evenement : l'ecran de repos");
        TEST_ASSERT(v.title != NULL && v.line != NULL, "aucun texte n'est NULL");
    }
}

/* Chaque operation a son libelle, et deux operations n'en partagent jamais un —
 * sinon l'ecran ne distingue pas une signature d'un dechiffrement.
 *
 * SEC_OP_UNKNOWN est dans ce tableau, pas seulement dans les quatre connues :
 * sans elle, ce test ne peut jamais voir une mutation qui ferait fuiter le
 * libelle d'une operation connue vers le `default:` (l'inconnu) — il
 * comparerait les quatre connues entre elles et s'arreterait la, aveugle a
 * exactement le cas que screen_op_label() existe pour distinguer. */
static void test_every_op_has_a_distinct_label(void)
{
    const sec_op_t ops[] = { SEC_OP_UNKNOWN, SEC_OP_SIGN, SEC_OP_DECRYPT, SEC_OP_AUTH, SEC_OP_OTP };
    const unsigned n = sizeof(ops) / sizeof(ops[0]);
    for (unsigned i = 0; i < n; i++) {
        const char *a = screen_view_of(USB_MODE_PGP, true, ops[i], LED_EVENT_NONE).line;
        TEST_ASSERT(a != NULL && a[0] != '\0', "chaque operation a un libelle non vide");
        for (unsigned j = i + 1; j < n; j++) {
            const char *b = screen_view_of(USB_MODE_PGP, true, ops[j], LED_EVENT_NONE).line;
            TEST_ASSERT(strcmp(a, b) != 0, "deux operations ne partagent pas un libelle");
        }
    }
}

/* Une operation inconnue ne doit pas etre presentee comme une operation connue. */
static void test_unknown_op_is_not_mistaken_for_a_known_one(void)
{
    const char *u = screen_view_of(USB_MODE_PGP, true, SEC_OP_UNKNOWN, LED_EVENT_NONE).line;
    const char *s = screen_view_of(USB_MODE_PGP, true, SEC_OP_SIGN, LED_EVENT_NONE).line;
    TEST_ASSERT(strcmp(u, s) != 0, "l'operation inconnue a son propre libelle");
}

/* L'attente prime sur la bascule : c'est le seul ecran qui reclame une action. */
static void test_wait_beats_switch(void)
{
    screen_view_t v = screen_view_of(USB_MODE_PGP, true, SEC_OP_SIGN, LED_EVENT_MODE);
    TEST_ASSERT_EQ(v.kind, SCREEN_WAIT, "l'attente prime sur la bascule");
}

/* Le verdict prime sur la bascule : il est fugace et c'est ce qu'on cherche a lire. */
static void test_verdict_beats_switch(void)
{
    screen_view_t v = screen_view_of(USB_MODE_PGP, false, SEC_OP_UNKNOWN, LED_EVENT_GRANTED);
    TEST_ASSERT_EQ(v.kind, SCREEN_VERDICT, "le verdict prime sur la bascule");
}

/* L'attente prime aussi sur un verdict encore actif — epingle le choix fait
 * dans screen_view_of() (confirm_pending teste avant event). Si un verdict
 * est encore affiche au moment ou une nouvelle confirmation s'arme, ce
 * verdict n'est jamais montre : rater l'attente coute une operation entiere
 * a l'utilisateur, rater un verdict ne coute qu'une information deja
 * redondante avec la LED. Reste a savoir si les deux peuvent coexister une
 * fois l'instantane publie sous verrou (tache 4) — ce test fixe seulement ce
 * que cette fonction pure decide aujourd'hui. */
static void test_wait_beats_pending_verdict(void)
{
    screen_view_t v = screen_view_of(USB_MODE_PGP, true, SEC_OP_SIGN, LED_EVENT_GRANTED);
    TEST_ASSERT_EQ(v.kind, SCREEN_WAIT, "l'attente prime sur un verdict encore actif");
}

/* Accord et refus doivent etre distinguables en toutes lettres. */
static void test_granted_and_refused_read_differently(void)
{
    const char *g = screen_view_of(USB_MODE_PGP, false, SEC_OP_UNKNOWN, LED_EVENT_GRANTED).title;
    const char *r = screen_view_of(USB_MODE_PGP, false, SEC_OP_UNKNOWN, LED_EVENT_REFUSED).title;
    TEST_ASSERT(strcmp(g, r) != 0, "accord et refus ne s'ecrivent pas pareil");
}

/* Au repos, l'ecran nomme le mode — c'est ce qui leve l'ambiguite du rouge.
 * Et deux modes quelconques n'en partagent jamais un, meme trou que pour les
 * libelles d'operation transpose au nom de mode : une seule paire (PGP vs
 * OTP) ne voit pas une mutation qui ferait fuiter le nom d'un mode connu
 * vers un autre, ou vers le hors-enum jamais compare a rien. Le tableau
 * couvre les six valeurs — NONE, STORAGE, PGP, OTP, FIDO, et USB_MODE_COUNT
 * (hors enum) — et compare chaque paire. */
static void test_every_mode_has_a_distinct_name(void)
{
    const usb_mode_t modes[] = { USB_MODE_NONE, USB_MODE_STORAGE, USB_MODE_PGP,
                                  USB_MODE_OTP, USB_MODE_FIDO, USB_MODE_COUNT };
    const unsigned n = sizeof(modes) / sizeof(modes[0]);
    for (unsigned i = 0; i < n; i++) {
        const char *a = screen_view_of(modes[i], false, SEC_OP_UNKNOWN, LED_EVENT_NONE).title;
        TEST_ASSERT(a != NULL && a[0] != '\0', "chaque mode a un nom non vide");
        for (unsigned j = i + 1; j < n; j++) {
            const char *b = screen_view_of(modes[j], false, SEC_OP_UNKNOWN, LED_EVENT_NONE).title;
            TEST_ASSERT(strcmp(a, b) != 0, "deux modes ne partagent pas un nom");
        }
    }
}

void test_screen_view(void)
{
    TEST_SUITE("screen_view");
    TEST_RUN(test_every_state_has_a_screen);
    TEST_RUN(test_every_op_has_a_distinct_label);
    TEST_RUN(test_unknown_op_is_not_mistaken_for_a_known_one);
    TEST_RUN(test_wait_beats_switch);
    TEST_RUN(test_verdict_beats_switch);
    TEST_RUN(test_wait_beats_pending_verdict);
    TEST_RUN(test_granted_and_refused_read_differently);
    TEST_RUN(test_every_mode_has_a_distinct_name);
}
