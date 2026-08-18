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

#include <string.h>

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
/* Les points de cycle — DERIVES du cycle, pas de l'enumeration.             */
/* ------------------------------------------------------------------------- */

/*
 * Erreur de conception corrigee (2026-08-18) : screen_mode_count() rendait le
 * nombre de VALEURS de usb_mode_t (5), alors que le cycle de cette carte
 * (usb_mode_cycle.h : pgp -> otp -> fido -> pgp) ne fait que TROIS crans —
 * elle n'a pas de microSD, et USB_MODE_NONE n'y revient jamais. Compter les
 * modes de l'enum et compter les crans du cycle sont deux choses
 * differentes ; la confusion etait invisible tant qu'un cycle couvrait tous
 * les modes.
 *
 * Valeur en dur et non usb_mode_cycle_after() rejoue a la main : ce test
 * epingle une DECISION observable (combien de points sur la dalle), pas
 * l'implementation du cycle qu'il verifie justement par un autre chemin.
 */
static void test_mode_count_matches_the_cycle_length(void)
{
    TEST_ASSERT_EQ(screen_mode_count(), 3, "trois crans dans le cycle de cette carte");
}

/* Chaque mode DU CYCLE a SON point, et deux modes n'en partagent jamais un :
 * sinon le point plein designerait deux modes a la fois et ne dirait plus ou
 * l'on est. Les trois valeurs sont comparees deux a deux — trois paires — et
 * non chacune a elle-meme : une mutation faisant rendre l'indice de PGP par
 * OTP passerait n'importe quelle formulation plus faible. */
static void test_cycle_modes_have_distinct_pairwise_dots(void)
{
    const usb_mode_t modes[] = { USB_MODE_PGP, USB_MODE_OTP, USB_MODE_FIDO };
    const unsigned n = sizeof(modes) / sizeof(modes[0]);
    for (unsigned i = 0; i < n; i++) {
        const uint8_t a = screen_mode_index(modes[i]);
        TEST_ASSERT(a < screen_mode_count(), "l'indice d'un mode du cycle designe un point");
        for (unsigned j = i + 1; j < n; j++) {
            TEST_ASSERT(a != screen_mode_index(modes[j]),
                        "deux modes du cycle ne partagent pas un point");
        }
    }
}

/*
 * Un mode valide de l'enum mais HORS DU CYCLE de cette carte (STORAGE : pas
 * de microSD ici) n'allume aucun point — comme un mode carrement aberrant.
 * Comparee a une valeur DISTINCTE (l'indice reel de PGP), pas a elle-meme :
 * sans ca, une mutation qui ferait rendre screen_mode_count() par accident
 * pour STORAGE ET pour PGP passerait inapercue.
 */
static void test_off_cycle_mode_lights_no_dot(void)
{
    const uint8_t storage_idx = screen_mode_index(USB_MODE_STORAGE);
    const uint8_t pgp_idx     = screen_mode_index(USB_MODE_PGP);

    TEST_ASSERT_EQ(storage_idx, screen_mode_count(),
                   "storage n'est pas dans le cycle de cette carte : aucun point");
    TEST_ASSERT(storage_idx != pgp_idx,
                "le sentinel « aucun point » n'est pas confondu avec un vrai point");
}

/* Un mode aberrant n'allume AUCUN point plutot que celui du repos : meme
 * principe que screen_mode_name(), qui rend « MODE INCONNU » et non le nom
 * d'un mode connu. Un indice replie sur 0 ferait passer une valeur hors enum
 * pour « rien expose ». */
static void test_unknown_mode_lights_no_dot(void)
{
    const usb_mode_t modes[] = { USB_MODE_PGP, USB_MODE_OTP, USB_MODE_FIDO };
    const uint8_t out_of_enum = screen_mode_index(USB_MODE_COUNT);
    const uint8_t nonsense    = screen_mode_index((usb_mode_t)99);

    TEST_ASSERT_EQ(out_of_enum, screen_mode_count(), "hors enum : aucun point");
    TEST_ASSERT_EQ(nonsense, screen_mode_count(), "valeur absurde : aucun point");
    for (unsigned i = 0; i < 3u; i++) {
        TEST_ASSERT(out_of_enum != screen_mode_index(modes[i]),
                    "l'inconnu ne prend pas le point d'un mode connu");
    }
}

/*
 * Garde anti-boucle infinie de screen_cycle_count() : si le cycle injecte ne
 * revient JAMAIS a son point de depart (ici, un « suivant » qui reste
 * coince ailleurs), le parcours doit tout de meme TERMINER — borne a
 * USB_MODE_COUNT iterations — et rendre une valeur sure (screen_mode_count()
 * hors de tout appel reel, ici directement le sentinel USB_MODE_COUNT) plutot
 * que de boucler indefiniment. Impossible a prouver avec le VRAI cycle de
 * cette carte, qui revient toujours a son depart en trois pas et masquerait
 * n'importe quelle valeur de la borne : c'est pour ceci que
 * screen_cycle_count() prend le « suivant » en parametre plutot que d'appeler
 * usb_mode_cycle_after() en dur.
 */
static usb_mode_t stuck_away_from_start(usb_mode_t current)
{
    (void)current;
    /* Ne rend jamais USB_MODE_PGP (le point de depart choisi par le test) :
     * un cycle qui boucle sur un sous-ensemble n'incluant pas le depart. */
    return USB_MODE_STORAGE;
}

static void test_cycle_count_terminates_when_the_cycle_never_returns(void)
{
    const uint8_t count = screen_cycle_count(USB_MODE_PGP, stuck_away_from_start);
    TEST_ASSERT_EQ(count, USB_MODE_COUNT,
                   "un cycle qui ne revient jamais au depart rend le sentinel, pas une boucle infinie");
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

/* ------------------------------------------------------------------------- */
/* Libelles courts, pour la police double hauteur.                            */
/* ------------------------------------------------------------------------- */

/*
 * Le test qui compte n'est pas le mappage mais la LARGEUR.
 *
 * En police double hauteur (12 px par cellule) un ecran de 128 px tient dix
 * caracteres. screen_op_label() en rend jusqu'a dix-huit (« Operation
 * inconnue ») : il fallait des libellés courts. Rien dans le compilateur ne
 * relie la longueur d'une chaine a la largeur d'une police — quelqu'un qui
 * rallonge un libelle pour le rendre plus clair casserait l'affichage en
 * silence, et le defaut ne se verrait que sur la dalle, tronque en plein milieu
 * d'un glyphe.
 */
#define SCREEN_BIG_CHAR_PX (2u * SCREEN_CHAR_PX)

static void test_op_short_fits_the_double_height_font(void)
{
    const sec_op_t ops[] = { SEC_OP_UNKNOWN, SEC_OP_SIGN, SEC_OP_DECRYPT,
                             SEC_OP_AUTH, SEC_OP_OTP };
    for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        const char *s = screen_op_short(ops[i]);
        TEST_ASSERT(s != NULL, "aucun libelle court n'est nul");
        TEST_ASSERT(screen_text_px(s) * 2u <= 128u,
                    "le libelle court tient en double hauteur sur 128 px");
    }
    /* Et la borne est bien serree : un onzieme caractere ne tiendrait pas.
     * Sans cette assertion, la precedente resterait vraie meme si la police
     * doublait de largeur. */
    TEST_ASSERT(screen_text_px("12345678901") * 2u > 128u,
                "onze caracteres en double hauteur depassent 128 px");
}

static void test_op_short_maps_each_operation(void)
{
    /* Comparaisons deux a deux, jamais une valeur contre elle-meme : c'est le
     * defaut recurrent de ce projet — un test qui croit couvrir cinq cas alors
     * qu'il n'en compare qu'un a lui-meme. */
    const char *unk  = screen_op_short(SEC_OP_UNKNOWN);
    const char *sign = screen_op_short(SEC_OP_SIGN);
    const char *dec  = screen_op_short(SEC_OP_DECRYPT);
    const char *auth = screen_op_short(SEC_OP_AUTH);
    const char *otp  = screen_op_short(SEC_OP_OTP);

    TEST_ASSERT(strcmp(sign, dec) != 0,  "signer et dechiffrer se distinguent");
    TEST_ASSERT(strcmp(sign, auth) != 0, "signer et authentifier se distinguent");
    TEST_ASSERT(strcmp(sign, otp) != 0,  "signer et OTP se distinguent");
    TEST_ASSERT(strcmp(dec, auth) != 0,  "dechiffrer et authentifier se distinguent");
    TEST_ASSERT(strcmp(dec, otp) != 0,   "dechiffrer et OTP se distinguent");
    TEST_ASSERT(strcmp(auth, otp) != 0,  "authentifier et OTP se distinguent");
    TEST_ASSERT(strcmp(unk, sign) != 0,  "l'inconnu ne se dit pas comme signer");
    TEST_ASSERT(strcmp(unk, dec) != 0,   "l'inconnu ne se dit pas comme dechiffrer");
    TEST_ASSERT(strcmp(unk, auth) != 0,  "l'inconnu ne se dit pas comme authentifier");
    TEST_ASSERT(strcmp(unk, otp) != 0,   "l'inconnu ne se dit pas comme OTP");
}

/*
 * Une valeur hors enum doit dire « inconnu », pas emprunter le libelle d'une
 * operation reelle. Sur l'ecran qui annonce ce qu'on autorise, afficher
 * « SIGNATURE » pour un code d'operation aberrant ferait confirmer autre chose
 * que ce qui est montre.
 */
static void test_op_short_out_of_range_says_unknown(void)
{
    TEST_ASSERT(strcmp(screen_op_short((sec_op_t)99), screen_op_short(SEC_OP_UNKNOWN)) == 0,
                "une valeur aberrante se dit comme l'inconnu");
    TEST_ASSERT(strcmp(screen_op_short((sec_op_t)99), screen_op_short(SEC_OP_SIGN)) != 0,
                "une valeur aberrante ne se dit PAS comme signer");
}

/* ------------------------------------------------------------------------- */
/* Logo errant de la veille.                                                  */
/* ------------------------------------------------------------------------- */

/*
 * Mae voulait le logo en veille plutot qu'un ecran noir. Un logo FIXE se
 * graverait dans la dalle ; en le promenant, chaque pixel n'est allume qu'une
 * fraction du temps. screen_wander() rend une coordonnee en dents de scie, et
 * ce qui doit etre garanti n'est pas une valeur precise mais une BORNE : jamais
 * au-dela de `span`, sinon le logo sort du cadre et se fait couper.
 */
static void test_wander_stays_within_bounds(void)
{
    /* Balayage large, avec un pas qui n'est un diviseur d'aucune des deux
     * periodes : un pas de 1000 ms tomberait toujours sur des instants ronds et
     * ne verrait jamais l'interieur des rampes. */
    for (uint32_t t = 0; t < 400000u; t += 137u) {
        TEST_ASSERT(screen_wander(t, SCREEN_WANDER_X_MS, 96u) <= 96u,
                    "l'abscisse errante reste dans la dalle");
        TEST_ASSERT(screen_wander(t, SCREEN_WANDER_Y_MS, 23u) <= 23u,
                    "l'ordonnee errante reste dans la dalle");
    }
}

/* Les deux extremes sont ATTEINTS : une fonction qui rendrait toujours 0
 * passerait le test de bornes ci-dessus sans rien promener. */
static void test_wander_reaches_both_ends(void)
{
    TEST_ASSERT_EQ(screen_wander(0u, 1000u, 96u), 0, "depart a l'origine");
    TEST_ASSERT_EQ(screen_wander(1000u, 1000u, 96u), 96, "extreme atteint a la demi-periode");
    TEST_ASSERT_EQ(screen_wander(2000u, 1000u, 96u), 0, "retour a l'origine a la periode pleine");
    TEST_ASSERT_EQ(screen_wander(500u, 1000u, 96u), 48, "milieu de la rampe montante");
    TEST_ASSERT_EQ(screen_wander(1500u, 1000u, 96u), 48, "milieu de la rampe descendante");
}

/* Symetrie de la dent de scie : l'aller et le retour passent par les memes
 * points. Une mutation qui casserait la rampe descendante (en la faisant monter
 * aussi) laisserait les bornes intactes mais briserait ceci. */
static void test_wander_is_symmetric(void)
{
    const uint32_t p = 1000u;
    for (uint32_t i = 0; i <= p; i += 7u) {
        TEST_ASSERT_EQ(screen_wander(i, p, 96u), screen_wander(2u * p - i, p, 96u),
                       "l'aller et le retour se superposent");
    }
}

/*
 * Une periode nulle ne divise pas par zero.
 *
 * Inatteignable avec les constantes du projet — et c'est exactement pourquoi
 * rien d'autre que ce test ne le protege. Une division par zero sur un P4 ne
 * lance pas d'exception : elle rend un resultat indefini, donc un logo place
 * n'importe ou, y compris hors cadre.
 */
static void test_wander_survives_zero_period(void)
{
    TEST_ASSERT_EQ(screen_wander(12345u, 0u, 96u), 0, "periode nulle : pas de division");
    TEST_ASSERT_EQ(screen_wander(0u, 0u, 96u), 0, "periode nulle a l'instant zero");
}

static void test_wander_zero_span_is_immobile(void)
{
    for (uint32_t t = 0; t < 5000u; t += 311u) {
        TEST_ASSERT_EQ(screen_wander(t, 1000u, 0u), 0, "aucune place : aucun deplacement");
    }
}

/* Les deux axes ne doivent pas avancer du meme pas, sinon le logo suit une
 * diagonale et ne visite qu'une bande de la dalle — la brulure se concentrerait
 * sur cette bande au lieu de se repartir. */
static void test_wander_axes_have_different_periods(void)
{
    TEST_ASSERT(SCREEN_WANDER_X_MS != SCREEN_WANDER_Y_MS,
                "les deux axes ont des periodes distinctes");
    /* Et pas un simple multiple l'un de l'autre, ce qui refermerait le trajet
     * sur lui-meme au bout de deux periodes. */
    TEST_ASSERT(SCREEN_WANDER_X_MS % SCREEN_WANDER_Y_MS != 0u,
                "l'une n'est pas un multiple de l'autre");
    TEST_ASSERT(SCREEN_WANDER_Y_MS % SCREEN_WANDER_X_MS != 0u,
                "ni l'inverse");
}

static void test_wander_survives_millisecond_wraparound(void)
{
    const uint32_t p = 1000u;
    /* A cheval sur le repassage a zero : la dent de scie doit rester continue,
     * donc bornee, sans saut hors cadre. */
    for (uint32_t i = 0; i < 4000u; i += 13u) {
        const uint32_t t = 0xFFFFFF00u + i;   /* repasse par zero en cours de route */
        TEST_ASSERT(screen_wander(t, p, 96u) <= 96u,
                    "borne tenue a cheval sur le repassage a zero");
    }
}


/*
 * Extinction franche, APRES la veille.
 *
 * Le test qui compte n'est pas le seuil mais son ORDRE. Si l'extinction
 * arrivait avant ou en meme temps que la veille, le logo errant que Mae a
 * demande ne serait jamais visible une seule image — et rien dans le
 * compilateur ne relie ces deux constantes.
 */
static void test_sleep_comes_strictly_after_standby(void)
{
    TEST_ASSERT(SCREEN_SLEEP_AFTER_MS > SCREEN_BLANK_AFTER_MS,
                "l'extinction arrive apres la veille, jamais avant");

    /* Et il existe bien un intervalle ou l'on est en veille SANS etre eteint :
     * c'est la fenetre pendant laquelle le logo se promene. */
    const uint32_t t0 = 500u;
    const uint32_t mid = t0 + SCREEN_BLANK_AFTER_MS
                       + (SCREEN_SLEEP_AFTER_MS - SCREEN_BLANK_AFTER_MS) / 2u;
    TEST_ASSERT(screen_blank_after_ms(t0, mid), "en veille au milieu de la fenetre");
    TEST_ASSERT(!screen_sleep_after_ms(t0, mid), "mais pas encore eteint");
}

static void test_sleep_bounds_are_exact(void)
{
    const uint32_t t0 = 1234u;
    TEST_ASSERT(!screen_sleep_after_ms(t0, t0), "allume a l'instant zero");
    TEST_ASSERT(!screen_sleep_after_ms(t0, t0 + SCREEN_SLEEP_AFTER_MS - 1u),
                "encore allume une milliseconde avant");
    TEST_ASSERT(screen_sleep_after_ms(t0, t0 + SCREEN_SLEEP_AFTER_MS),
                "eteint pile au seuil");
    TEST_ASSERT(screen_sleep_after_ms(t0, t0 + SCREEN_SLEEP_AFTER_MS + 60000u),
                "toujours eteint bien apres");
}

static void test_sleep_survives_millisecond_wraparound(void)
{
    const uint32_t t0 = 0xFFFFFFFFu - (SCREEN_SLEEP_AFTER_MS / 2u);
    TEST_ASSERT(!screen_sleep_after_ms(t0, t0 + SCREEN_SLEEP_AFTER_MS - 1u),
                "allume juste avant le seuil, a cheval sur le repassage a zero");
    TEST_ASSERT(screen_sleep_after_ms(t0, t0 + SCREEN_SLEEP_AFTER_MS),
                "eteint au seuil, a cheval sur le repassage a zero");
}


/* ------------------------------------------------------------------------- */
/* Course disponible pour un element mobile.                                  */
/* ------------------------------------------------------------------------- */

/*
 * screen_span() est la garde que render_standby() n'avait pas.
 *
 * Il calculait « largeur de l'ecran - largeur du groupe » a la main. Si le
 * groupe depassait l'ecran, la soustraction non signee se repliait sur des
 * dizaines de milliers et le logo partait se promener hors champ — defaisant
 * precisement la fonction anti-marquage pour laquelle la veille existe.
 *
 * Meme faute que screen_center_x() documente et evite depuis le debut ; la
 * revue de branche l'a trouvee a un pixel pres au meme endroit.
 */
static void test_span_is_the_room_left(void)
{
    TEST_ASSERT_EQ(screen_span(128, 32), 96, "un logo de 32 sur 128 : 96 de course");
    TEST_ASSERT_EQ(screen_span(64, 41), 23, "un groupe de 41 sur 64 : 23 de course");
    TEST_ASSERT_EQ(screen_span(128, 128), 0, "pile la largeur : aucune course");
    TEST_ASSERT_EQ(screen_span(128, 0), 128, "rien a placer : toute la course");
}

static void test_span_never_wraps(void)
{
    TEST_ASSERT_EQ(screen_span(128, 129), 0, "un pixel de trop : zero, pas 65535");
    TEST_ASSERT_EQ(screen_span(64, 200), 0, "tres au-dela : zero");
    TEST_ASSERT_EQ(screen_span(0, 32), 0, "ecran de largeur nulle : zero");
    TEST_ASSERT_EQ(screen_span(128, 0xFFFF), 0, "largeur saturee : zero");

    /* Formule en borne, sans valeur attendue precise : un repli non signe la
     * casserait quelle que soit la facon de l'ecrire. */
    for (uint32_t used = 0; used <= 400u; used += 7u) {
        TEST_ASSERT(screen_span(128, (uint16_t)used) <= 128,
                    "la course ne depasse jamais l'ecran");
    }
}

/*
 * Et le lien avec l'errance : une course nulle doit immobiliser, pas teleporter.
 * C'est la composition des deux fonctions qui protege reellement l'affichage,
 * pas chacune isolement.
 */
static void test_span_zero_keeps_the_logo_still(void)
{
    const uint16_t span = screen_span(64, 200);
    for (uint32_t t = 0; t < 120000u; t += 997u) {
        TEST_ASSERT_EQ(screen_wander(t, SCREEN_WANDER_X_MS, span), 0,
                       "sans course, l'element ne bouge pas");
    }
}

void test_screen_layout(void)
{
    TEST_SUITE("screen_layout");
    TEST_RUN(test_text_px_counts_six_pixels_per_char);
    TEST_RUN(test_text_px_tolerates_null);
    TEST_RUN(test_text_px_saturates_instead_of_wrapping);
    TEST_RUN(test_center_x_centers);
    TEST_RUN(test_center_x_never_wraps_when_text_is_wider);
    TEST_RUN(test_mode_count_matches_the_cycle_length);
    TEST_RUN(test_cycle_modes_have_distinct_pairwise_dots);
    TEST_RUN(test_off_cycle_mode_lights_no_dot);
    TEST_RUN(test_unknown_mode_lights_no_dot);
    TEST_RUN(test_cycle_count_terminates_when_the_cycle_never_returns);
    TEST_RUN(test_seconds_left_rounds_up);
    TEST_RUN(test_seconds_left_matches_the_ceiling_everywhere);
    TEST_RUN(test_seconds_left_survives_millisecond_wraparound);
    TEST_RUN(test_verdict_glyph_maps_each_event);
    TEST_RUN(test_verdict_glyphs_are_three_distinct_things);
    TEST_RUN(test_blank_only_after_a_full_minute);
    TEST_RUN(test_blank_survives_millisecond_wraparound);
    TEST_RUN(test_op_short_fits_the_double_height_font);
    TEST_RUN(test_op_short_maps_each_operation);
    TEST_RUN(test_op_short_out_of_range_says_unknown);
    TEST_RUN(test_wander_stays_within_bounds);
    TEST_RUN(test_wander_reaches_both_ends);
    TEST_RUN(test_wander_is_symmetric);
    TEST_RUN(test_wander_survives_zero_period);
    TEST_RUN(test_wander_zero_span_is_immobile);
    TEST_RUN(test_wander_axes_have_different_periods);
    TEST_RUN(test_wander_survives_millisecond_wraparound);
    TEST_RUN(test_sleep_comes_strictly_after_standby);
    TEST_RUN(test_sleep_bounds_are_exact);
    TEST_RUN(test_sleep_survives_millisecond_wraparound);
    TEST_RUN(test_span_is_the_room_left);
    TEST_RUN(test_span_never_wraps);
    TEST_RUN(test_span_zero_keeps_the_logo_still);
}
