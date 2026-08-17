/* Ce que la disposition de l'ecran decide, et que le trace ne doit pas decider
 * a sa place : la largeur d'un texte, son origine centree, la position dans le
 * cycle de modes, le nombre de secondes restantes, le glyphe de verdict, et
 * l'extinction anti-remanence.
 *
 * Aucun de ces calculs ne connait la geometrie de l'ecran : elle arrive en
 * parametre (screen_center_x) ou n'intervient pas du tout. C'est ce qui les
 * rend testables ici, et c'est aussi ce qui empeche screen.c de les melanger
 * a des coordonnees en dur — le defaut exact que la revue de l'ecran a
 * releve : « tout en haut a gauche, aucun centrage ».
 */
#include "test_framework.h"

#include "sec_confirm.h"
#include "usb/usb_mode.h"
#include "hmi/led_state.h"
#include "hmi/screen_layout.h"

/* ------------------------------------------------------------------------- */
/* Largeur d'un texte.                                                        */
/* ------------------------------------------------------------------------- */

static void test_text_px_counts_six_pixels_per_char(void)
{
    TEST_ASSERT_EQ(screen_text_px(""), 0, "un texte vide n'occupe rien");
    TEST_ASSERT_EQ(screen_text_px("A"), 6, "un caractere occupe une cellule");
    TEST_ASSERT_EQ(screen_text_px("AB"), 12, "deux caracteres, deux cellules");
    TEST_ASSERT_EQ(screen_text_px("SIGNATURE"), 54, "neuf caracteres");
    TEST_ASSERT_EQ(screen_text_px("DECHIFFRER"), 60, "dix caracteres");
    /* L'espace compte comme un caractere : sinon « CLE OTP » se centrerait
     * comme « CLEOTP » et pencherait a droite. */
    TEST_ASSERT_EQ(screen_text_px("CLE OTP"), 42, "l'espace occupe sa cellule");
}

/* NULL n'est pas un texte vide par accident : screen_view_t promet des chaines
 * non nulles, mais cette fonction sert aussi a mesurer un tampon local, et un
 * NULL doit rendre 0 plutot que dereferencer. */
static void test_text_px_tolerates_null(void)
{
    TEST_ASSERT_EQ(screen_text_px(NULL), 0, "NULL ne vaut aucun pixel");
}

/* Un texte de plus de 10922 caracteres depasserait uint16_t : 10923 * 6 =
 * 65538, qui se replierait sur 2 — donc une chaine gigantesque se dirait
 * PLUS ETROITE qu'un seul caractere, et screen_center_x la centrerait au
 * milieu de l'ecran. La saturation est le seul comportement qui ne mente pas.
 * Inatteignable avec les chaines du projet ; c'est justement pourquoi rien
 * d'autre que ce test ne la protege. */
static void test_text_px_saturates_instead_of_wrapping(void)
{
    char big[11001];
    memset(big, 'A', sizeof(big) - 1u);
    big[sizeof(big) - 1u] = '\0';
    TEST_ASSERT_EQ(screen_text_px(big), 0xFFFF, "sature au maximum de uint16_t");

    /* Juste en dessous du seuil, la valeur reste exacte : la saturation ne
     * doit pas ecraser les largeurs normales. */
    big[10000] = '\0';
    TEST_ASSERT_EQ(screen_text_px(big), 60000, "exacte juste sous le seuil");
}

/* ------------------------------------------------------------------------- */
/* Origine centree.                                                           */
/* ------------------------------------------------------------------------- */

static void test_center_x_centers(void)
{
    TEST_ASSERT_EQ(screen_center_x(128, 60), 34, "dix caracteres centres sur 128 px");
    TEST_ASSERT_EQ(screen_center_x(128, 6), 61, "un caractere au milieu");
    TEST_ASSERT_EQ(screen_center_x(128, 0), 64, "un texte vide se centre au milieu");
    TEST_ASSERT_EQ(screen_center_x(128, 128), 0, "pile la largeur : colle a gauche");
    /* La police double hauteur fait 12 px par caractere : c'est l'appel reel
     * de l'ecran de confirmation, celui qui compte. */
    TEST_ASSERT_EQ(screen_center_x(128, 2u * 54u), 10, "SIGNATURE en 12x16, centre");
}

/* Le piege de l'arithmetique non signee : un texte plus large que l'ecran
 * donnerait (width - text) / 2 replie sur ~32767, et le texte serait dessine
 * hors champ a droite — invisible au lieu d'etre tronque. On rend 0 : le
 * texte commence a gauche et se coupe a droite, ce que fb_set_pixel() borne
 * deja. */
static void test_center_x_never_wraps_when_text_is_wider(void)
{
    TEST_ASSERT_EQ(screen_center_x(128, 132), 0, "plus large que l'ecran : origine a gauche");
    TEST_ASSERT_EQ(screen_center_x(128, 0xFFFF), 0, "largeur saturee : origine a gauche");
    TEST_ASSERT_EQ(screen_center_x(64, 65), 0, "un pixel de trop suffit");
    TEST_ASSERT_EQ(screen_center_x(0, 6), 0, "un ecran de largeur nulle ne replie pas non plus");

    /* Le meme constat, formule comme une borne : jamais au-dela de l'ecran,
     * quelle que soit la largeur du texte. Un repli non signe casserait
     * celle-ci aussi, et elle ne depend d'aucune valeur attendue precise. */
    for (uint32_t t = 0; t <= 300u; t += 3u) {
        TEST_ASSERT(screen_center_x(128, (uint16_t)t) <= 128,
                    "l'origine reste dans l'ecran");
    }
}

/* ------------------------------------------------------------------------- */
/* Les quatre points de cycle.                                                */
/* ------------------------------------------------------------------------- */

/* Valeur en dur et non USB_MODE_COUNT : ce test epingle une DECISION de
 * disposition (quatre points tiennent dans 128 px de large), pas l'enum. */
static void test_mode_count_is_four(void)
{
    TEST_ASSERT_EQ(screen_mode_count(), 4, "quatre points de cycle");
}

/* Chaque mode a SON point, et deux modes n'en partagent jamais un : sinon le
 * point plein designerait deux modes a la fois et ne dirait plus ou l'on est.
 * Les quatre valeurs sont comparees deux a deux — six paires — et non chacune
 * a elle-meme : une mutation faisant rendre l'indice de PGP par STORAGE
 * passerait n'importe quelle formulation plus faible. */
static void test_each_mode_has_its_own_dot(void)
{
    const usb_mode_t modes[] = { USB_MODE_NONE, USB_MODE_STORAGE, USB_MODE_PGP, USB_MODE_OTP };
    const unsigned n = sizeof(modes) / sizeof(modes[0]);
    for (unsigned i = 0; i < n; i++) {
        const uint8_t a = screen_mode_index(modes[i]);
        TEST_ASSERT(a < screen_mode_count(), "l'indice d'un mode connu designe un point");
        for (unsigned j = i + 1; j < n; j++) {
            TEST_ASSERT(a != screen_mode_index(modes[j]),
                        "deux modes ne partagent pas un point");
        }
    }
}

/* Un mode aberrant n'allume AUCUN point plutot que celui du repos : meme
 * principe que screen_mode_name(), qui rend « MODE INCONNU » et non le nom
 * d'un mode connu. Un indice replie sur 0 ferait passer une valeur hors enum
 * pour « rien expose ». */
static void test_unknown_mode_lights_no_dot(void)
{
    const usb_mode_t modes[] = { USB_MODE_NONE, USB_MODE_STORAGE, USB_MODE_PGP, USB_MODE_OTP };
    const uint8_t out_of_enum = screen_mode_index(USB_MODE_COUNT);
    const uint8_t nonsense    = screen_mode_index((usb_mode_t)99);

    TEST_ASSERT_EQ(out_of_enum, screen_mode_count(), "hors enum : aucun point");
    TEST_ASSERT_EQ(nonsense, screen_mode_count(), "valeur absurde : aucun point");
    for (unsigned i = 0; i < 4u; i++) {
        TEST_ASSERT(out_of_enum != screen_mode_index(modes[i]),
                    "l'inconnu ne prend pas le point d'un mode connu");
    }
}

/* ------------------------------------------------------------------------- */
/* Secondes restantes.                                                        */
/* ------------------------------------------------------------------------- */

/* Arrondi VERS LE HAUT, et c'est le point entier de cette fonction : afficher
 * « 0 s » alors qu'il reste 900 ms dirait que c'est fini quand ca ne l'est pas
 * — le sens dangereux du mensonge. armed_at_ms n'est pas un multiple de 1000 :
 * un ancrage rond masquerait une erreur de phase. */
static void test_seconds_left_rounds_up(void)
{
    const uint32_t armed = 1234u;
    TEST_ASSERT_EQ(screen_seconds_left(armed, armed), 15, "quinze a l'armement");
    /* Une milliseconde ecoulee : la troncature dirait 14, l'arrondi vers le
     * haut dit 15. C'est ici que se voit la difference entre les deux. */
    TEST_ASSERT_EQ(screen_seconds_left(armed, armed + 1u), 15, "une ms plus tard : encore quinze");
    TEST_ASSERT_EQ(screen_seconds_left(armed, armed + 999u), 15, "juste avant la premiere seconde");
    TEST_ASSERT_EQ(screen_seconds_left(armed, armed + 1000u), 14, "une seconde pleine ecoulee");
    TEST_ASSERT_EQ(screen_seconds_left(armed, armed + 14000u), 1, "une seconde restante");
    /* 999 ms restantes : le cas cite par la specification. */
    TEST_ASSERT_EQ(screen_seconds_left(armed, armed + 14001u), 1, "999 ms restantes se disent « 1 s »");
    TEST_ASSERT_EQ(screen_seconds_left(armed, armed + 14999u), 1, "une ms restante se dit encore « 1 s »");
    TEST_ASSERT_EQ(screen_seconds_left(armed, armed + SEC_CONFIRM_TIMEOUT_MS), 0, "zero a l'echeance");
    TEST_ASSERT_EQ(screen_seconds_left(armed, armed + SEC_CONFIRM_TIMEOUT_MS + 9000u), 0,
                   "reste a zero apres l'echeance");
}

/* Les points ci-dessus sont choisis ; celui-ci balaye les 15001 valeurs contre
 * la definition de l'arrondi vers le haut. Meme raison que le balayage
 * exhaustif de screen_bar_permille : une erreur de denominateur ou de biais ne
 * diverge parfois que sur une milliseconde, et aucun jeu de points ronds ne
 * l'attrape. On y verifie aussi les deux proprietes que les valeurs seules ne
 * donnent pas — jamais zero avant l'echeance, et jamais de remontee. */
static void test_seconds_left_matches_the_ceiling_everywhere(void)
{
    const uint32_t armed = 1234u;
    uint16_t prev = screen_seconds_left(armed, armed);
    for (uint32_t d = 0; d <= SEC_CONFIRM_TIMEOUT_MS; d++) {
        const uint32_t remaining = SEC_CONFIRM_TIMEOUT_MS - d;
        const uint16_t expected  = (uint16_t)((remaining + 999u) / 1000u);
        const uint16_t got       = screen_seconds_left(armed, armed + d);
        TEST_ASSERT_EQ(got, expected, "correspond a l'arrondi vers le haut, milliseconde par milliseconde");
        if (remaining > 0u) {
            TEST_ASSERT(got >= 1u, "jamais zero tant qu'il reste du temps");
        }
        TEST_ASSERT(got <= prev, "le decompte ne remonte jamais");
        prev = got;
    }
}

/* Meme repassage a zero du compteur de millisecondes que les animations : une
 * cle branchee en permanence l'atteint apres ~49 jours. */
static void test_seconds_left_survives_millisecond_wraparound(void)
{
    const uint32_t armed = 0xFFFFF00Bu;   /* non aligne sur la seconde */
    TEST_ASSERT_EQ(screen_seconds_left(armed, armed), 15,
                   "quinze a l'armement, a cheval sur le repassage a zero");
    TEST_ASSERT_EQ(screen_seconds_left(armed, armed + 14999u), 1,
                   "une ms restante, a cheval sur le repassage a zero");
    TEST_ASSERT_EQ(screen_seconds_left(armed, armed + SEC_CONFIRM_TIMEOUT_MS), 0,
                   "zero a l'echeance, a cheval sur le repassage a zero");
}

/* ------------------------------------------------------------------------- */
/* Glyphe de verdict.                                                         */
/* ------------------------------------------------------------------------- */

static void test_verdict_glyph_maps_each_event(void)
{
    TEST_ASSERT_EQ(screen_verdict_glyph(LED_EVENT_GRANTED), SCREEN_VERDICT_CHECK,
                   "accorde : la coche");
    TEST_ASSERT_EQ(screen_verdict_glyph(LED_EVENT_REFUSED), SCREEN_VERDICT_CROSS,
                   "refuse : la croix");
    TEST_ASSERT_EQ(screen_verdict_glyph(LED_EVENT_NONE), SCREEN_VERDICT_NONE,
                   "aucun evenement : aucun glyphe");
    /* Une bascule de mode n'est pas un verdict : dessiner une coche parce que
     * le mode a change dirait qu'une operation a ete autorisee. */
    TEST_ASSERT_EQ(screen_verdict_glyph(LED_EVENT_MODE), SCREEN_VERDICT_NONE,
                   "une bascule de mode n'est pas un verdict");
    TEST_ASSERT_EQ(screen_verdict_glyph((led_event_t)99), SCREEN_VERDICT_NONE,
                   "un evenement aberrant ne dessine rien");
}

/* Les trois sorties comparees DEUX A DEUX, et non chacune a la constante
 * attendue : le test ci-dessus resterait vert si deux enumerateurs de
 * screen_verdict_kind_t partageaient la meme valeur — coche et croix
 * deviendraient interchangeables sans qu'aucune egalite ne bouge, et l'ecran
 * dirait « accorde » sur un refus. */
static void test_verdict_glyphs_are_three_distinct_things(void)
{
    const screen_verdict_kind_t check = screen_verdict_glyph(LED_EVENT_GRANTED);
    const screen_verdict_kind_t cross = screen_verdict_glyph(LED_EVENT_REFUSED);
    const screen_verdict_kind_t none  = screen_verdict_glyph(LED_EVENT_NONE);
    TEST_ASSERT(check != cross, "la coche n'est pas la croix");
    TEST_ASSERT(check != none, "la coche n'est pas l'absence de glyphe");
    TEST_ASSERT(cross != none, "la croix n'est pas l'absence de glyphe");
}

/* ------------------------------------------------------------------------- */
/* Extinction anti-remanence.                                                 */
/* ------------------------------------------------------------------------- */

/* Une minute, en dur : ce test epingle la DECISION (l'ecran s'eteint au bout
 * d'une minute d'inactivite), pas la macro. Ecrit avec SCREEN_BLANK_AFTER_MS
 * des deux cotes, il resterait vert si la constante passait a cinq secondes —
 * la dalle clignoterait sans arret et rien ne rougirait.
 * last_activity_ms n'est pas un multiple de la periode, pour la meme raison
 * que l'ancrage du decompte ci-dessus. */
static void test_blank_only_after_a_full_minute(void)
{
    const uint32_t last = 12345u;
    TEST_ASSERT(!screen_blank_after_ms(last, last), "allume a l'instant de l'activite");
    TEST_ASSERT(!screen_blank_after_ms(last, last + 30000u), "encore allume a trente secondes");
    TEST_ASSERT(!screen_blank_after_ms(last, last + 59999u),
                "encore allume une milliseconde avant la minute");
    TEST_ASSERT(screen_blank_after_ms(last, last + 60000u), "eteint pile a la minute");
    TEST_ASSERT(screen_blank_after_ms(last, last + 3600000u), "toujours eteint une heure apres");
}

static void test_blank_survives_millisecond_wraparound(void)
{
    const uint32_t last = 0xFFFFF00Bu;
    TEST_ASSERT(!screen_blank_after_ms(last, last + 59999u),
                "encore allume juste avant la minute, a cheval sur le repassage a zero");
    TEST_ASSERT(screen_blank_after_ms(last, last + 60000u),
                "eteint pile a la minute, a cheval sur le repassage a zero");
}

void test_screen_layout(void)
{
    TEST_SUITE("screen_layout");
    TEST_RUN(test_text_px_counts_six_pixels_per_char);
    TEST_RUN(test_text_px_tolerates_null);
    TEST_RUN(test_text_px_saturates_instead_of_wrapping);
    TEST_RUN(test_center_x_centers);
    TEST_RUN(test_center_x_never_wraps_when_text_is_wider);
    TEST_RUN(test_mode_count_is_four);
    TEST_RUN(test_each_mode_has_its_own_dot);
    TEST_RUN(test_unknown_mode_lights_no_dot);
    TEST_RUN(test_seconds_left_rounds_up);
    TEST_RUN(test_seconds_left_matches_the_ceiling_everywhere);
    TEST_RUN(test_seconds_left_survives_millisecond_wraparound);
    TEST_RUN(test_verdict_glyph_maps_each_event);
    TEST_RUN(test_verdict_glyphs_are_three_distinct_things);
    TEST_RUN(test_blank_only_after_a_full_minute);
    TEST_RUN(test_blank_survives_millisecond_wraparound);
}
