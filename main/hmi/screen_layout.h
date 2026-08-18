#pragma once

/*
 * screen_layout — ou poser ce que l'ecran montre, en logique pure.
 *
 * Troisieme en-tete pur de l'ecran, apres screen_view.h (quels mots) et
 * screen_anim.h (quelles proportions). Celui-ci porte ce qui restait melange
 * a des coordonnees en dur dans hmi/screen.c, et que la revue de l'ecran sur
 * dalle reelle a releve : « tout en haut a gauche », « aucun centrage »,
 * « tout de la meme taille ». Centrer, compter des points, arrondir des
 * secondes et decider une extinction sont des CALCULS ; seul le « ou poser le
 * pixel » reste dans screen.c.
 *
 * Ce module ne connait aucune dimension d'ecran : la largeur arrive en
 * parametre (screen_center_x), et rien ici ne suppose 128x64. La seule
 * geometrie qu'il porte est celle de la POLICE — 6 px par cellule — parce que
 * mesurer un texte sans connaitre sa police n'a pas de sens, et parce que
 * screen.c et ce fichier doivent avancer du meme pas (draw_text() s'en sert).
 *
 * Meme arithmetique de temps non signee que screen_anim.h : la difference
 * reste juste au repassage a zero du compteur de millisecondes, apres ~49
 * jours, qu'une cle branchee en permanence atteindra.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sec_confirm.h"
#include "usb/usb_mode.h"
#include "usb/usb_mode_cycle.h"
#include "hmi/led_state.h"

/* Cellule de la police 5x7 de screen.c : 5 colonnes de glyphe + 1 de marge.
 * La police double hauteur double cette valeur (12 px), par doublement de
 * pixels et sans table de glyphes supplementaire. */
#define SCREEN_CHAR_PX 6u

/* Extinction anti-remanence : une minute sans rien. Un OLED qui affiche le
 * meme contenu en continu garde une trace permanente, et le decalage de
 * screen_shift_px() ne fait que repartir la brulure sur quelques pixels de
 * plus — sans extinction, il ne la supprime pas. */
#define SCREEN_BLANK_AFTER_MS (60u * 1000u)

/* Extinction franche de la dalle, apres la veille. Le logo errant repartit la
 * brulure mais ne l'annule pas : sur une cle branchee des annees, il finit par
 * marquer. Une demi-heure laisse tout le temps de voir l'animation, et coupe
 * avant que ca compte. */
#define SCREEN_SLEEP_AFTER_MS (30u * 60u * 1000u)

typedef enum {
    SCREEN_VERDICT_NONE = 0,   /* rien a dessiner */
    SCREEN_VERDICT_CHECK,      /* la coche : accorde */
    SCREEN_VERDICT_CROSS,      /* la croix : refuse */
} screen_verdict_kind_t;

/*
 * Largeur d'un texte en pixels, police simple.
 *
 * Sature a UINT16_MAX au lieu de replier : 10923 caracteres feraient 65538,
 * soit 2 apres repli — une chaine gigantesque se dirait plus etroite qu'un
 * seul caractere, et screen_center_x() la centrerait tranquillement au milieu
 * de l'ecran. Inatteignable avec les chaines du projet, ce qui est justement
 * pourquoi rien d'autre qu'un test ne protege ce cas.
 *
 * NULL rend 0 : screen_view_t promet des chaines non nulles, mais cette
 * fonction sert aussi a mesurer un tampon local, et un NULL doit se mesurer
 * plutot que se dereferencer.
 */
static inline uint16_t screen_text_px(const char *s)
{
    if (s == NULL) {
        return 0;
    }
    uint32_t px = 0;
    for (const char *p = s; *p != '\0'; p++) {
        px += SCREEN_CHAR_PX;
        if (px >= 0xFFFFu) {
            return 0xFFFFu;
        }
    }
    return (uint16_t)px;
}

/*
 * Origine en X d'un texte centre sur `width_px`.
 *
 * Un texte plus large que l'ecran rend 0, jamais (width - text) / 2 : en
 * arithmetique non signee cette soustraction se replierait sur ~32767 et le
 * texte serait dessine hors champ a droite, donc INVISIBLE au lieu d'etre
 * tronque. Coller a gauche et laisser fb_set_pixel() couper a droite garde au
 * moins le debut du libelle lisible — et c'est le libelle qui dit pour quoi
 * on appuie.
 */
static inline uint16_t screen_center_x(uint16_t width_px, uint16_t text_px)
{
    if (text_px >= width_px) {
        return 0;
    }
    return (uint16_t)((width_px - text_px) / 2u);
}

/*
 * Nombre de points de cycle dessines et position de chacun : DERIVES du
 * CYCLE de la carte (usb_mode_cycle.h, usb_mode_cycle_after()), pas de
 * l'enumeration usb_mode_t.
 *
 * Erreur de conception corrigee (2026-08-18, ruling du coordinateur) :
 * compter les valeurs de l'enum et compter les crans du cycle sont deux
 * choses differentes. Elles coincidaient tant qu'un cycle couvrait tous les
 * modes ; la carte-cle (usb_mode_cycle.h) n'a que trois crans (pgp -> otp ->
 * fido -> pgp, pas de microSD, USB_MODE_NONE n'y revient jamais) sur cinq
 * valeurs d'enum. Deriver du cycle plutot que de l'enum rend ce fichier
 * juste pour N'IMPORTE QUELLE carte : un cycle different (ex. futur devkit)
 * change le nombre et la position des points sans toucher une ligne ici.
 *
 * screen_cycle_count() prend le "suivant" en parametre plutot que d'appeler
 * usb_mode_cycle_after() en dur : c'est ce qui permet de PROUVER par un test
 * que la garde anti-boucle infinie mord, avec un cycle volontairement casse
 * qui ne revient jamais a son point de depart — impossible a demontrer avec
 * le vrai cycle de cette carte, qui revient toujours en trois pas et
 * masquerait n'importe quelle valeur de la borne.
 */

/*
 * Parcourt le cycle `next` depuis `start` et compte les crans distincts
 * avant d'y revenir.
 *
 * Borne a USB_MODE_COUNT iterations. Le domaine de usb_mode_t est fini, donc
 * n'importe quelle fonction totale usb_mode_t -> usb_mode_t revient
 * forcement sur une valeur deja visitee avant ce nombre de pas (principe des
 * tiroirs) — mais rien ne garantit que cette valeur soit `start` lui-meme :
 * un cycle qui boucle sur un SOUS-ENSEMBLE n'incluant pas `start` ne
 * reviendrait jamais s'y arreter. Sans cette borne explicite, ce cas
 * bloquerait la tache d'affichage indefiniment ; avec elle, on rend une
 * valeur sure (USB_MODE_COUNT, deja le sentinel « hors du cycle » utilise
 * ailleurs dans ce fichier) plutot que de figer la carte.
 */
static inline uint8_t screen_cycle_count(usb_mode_t start, usb_mode_t (*next)(usb_mode_t))
{
    usb_mode_t m = start;
    for (uint8_t i = 0; i < USB_MODE_COUNT; i++) {
        m = next(m);
        if (m == start) {
            return (uint8_t)(i + 1u);
        }
    }
    return USB_MODE_COUNT;   /* jamais revenu au depart : valeur sure */
}

/*
 * Nombre de points de cycle dessines — le nombre de crans DISTINCTS du
 * cycle reel de cette carte, en partant du premier mode qu'un appui depuis
 * l'etat muet atteint (usb_mode_cycle_after(USB_MODE_NONE) : PGP sur cette
 * carte). Trois points tiennent toujours largement dans 128 px de large :
 * meme cinq le feraient deja (5 x 5 px de diametre + 4 x 7 px d'espacement
 * = 53 px), donc trois a fortiori.
 */
static inline uint8_t screen_mode_count(void)
{
    return screen_cycle_count(usb_mode_cycle_after(USB_MODE_NONE), usb_mode_cycle_after);
}

/*
 * Quel point est plein, de 0 a screen_mode_count() - 1 : la position de
 * `mode` dans le PARCOURS du cycle depuis son point de depart.
 *
 * Un mode hors du cycle de cette carte (STORAGE : pas de microSD ici) ou
 * hors enum rend screen_mode_count() : AUCUN point ne s'allume. Replier sur
 * 0 ferait passer l'aberration — ou un mode simplement non cyclable — pour
 * « rien expose » — meme principe que screen_mode_name(), qui rend « MODE
 * INCONNU » plutot que le nom d'un mode connu : un silence identique a un
 * etat connu masque l'erreur au lieu de la signaler.
 */
static inline uint8_t screen_mode_index(usb_mode_t mode)
{
    const usb_mode_t start = usb_mode_cycle_after(USB_MODE_NONE);
    const uint8_t    count = screen_cycle_count(start, usb_mode_cycle_after);

    usb_mode_t m = start;
    for (uint8_t i = 0; i < count; i++) {
        if (m == mode) {
            return i;
        }
        m = usb_mode_cycle_after(m);
    }
    return count;   /* `mode` n'est pas dans ce cycle */
}

/*
 * Secondes restantes avant l'expiration de la confirmation, arrondies VERS LE
 * HAUT.
 *
 * L'arrondi n'est pas un detail : afficher « 0 s » alors qu'il reste 900 ms
 * dirait que c'est fini quand ca ne l'est pas — le mensonge dans le sens
 * dangereux, sur l'affichage qui existe precisement parce que deux
 * generations de cles ont ete perdues sur des expirations invisibles. Le
 * chiffre passe donc a 0 exactement quand la barre se vide, jamais avant.
 */
static inline uint16_t screen_seconds_left(uint32_t armed_at_ms, uint32_t now_ms)
{
    const uint32_t elapsed = now_ms - armed_at_ms;   /* juste au repassage a zero */
    if (elapsed >= SEC_CONFIRM_TIMEOUT_MS) {
        return 0;
    }
    const uint32_t remaining = SEC_CONFIRM_TIMEOUT_MS - elapsed;
    return (uint16_t)((remaining + 999u) / 1000u);
}

/* Coche, croix, ou rien. Une bascule de mode n'est pas un verdict : dessiner
 * une coche parce que le mode a change dirait qu'une operation a ete
 * autorisee. */
static inline screen_verdict_kind_t screen_verdict_glyph(led_event_t event)
{
    switch (event) {
    case LED_EVENT_GRANTED: return SCREEN_VERDICT_CHECK;
    case LED_EVENT_REFUSED: return SCREEN_VERDICT_CROSS;
    default:                return SCREEN_VERDICT_NONE;
    }
}

/* La dalle doit-elle etre eteinte ? Vrai au bout de SCREEN_BLANK_AFTER_MS sans
 * activite — un appui, un evenement, ou une operation en attente. L'appelant
 * rallume au premier retour d'activite : voir hmi/screen.c. */
static inline bool screen_blank_after_ms(uint32_t last_activity_ms, uint32_t now_ms)
{
    const uint32_t idle = now_ms - last_activity_ms;   /* juste au repassage a zero */
    return idle >= SCREEN_BLANK_AFTER_MS;
}

/*
 * Libelle d'operation pour la police double hauteur : dix caracteres au plus.
 *
 * screen_op_label() (hmi/screen_view.h) rend jusqu'a dix-huit caracteres
 * (« Operation inconnue »), ce qui tient en police simple mais pas en double
 * hauteur : 12 px par cellule sur un ecran de 128 px n'en laisse passer que
 * dix. Ce sont donc deux libelles pour deux polices, pas une duplication —
 * l'ecran de confirmation montre celui-ci en grand, parce que c'est la seule
 * information dont la lecture a une consequence.
 *
 * Rien dans le compilateur ne relie la longueur d'une chaine a la largeur d'une
 * police : c'est test_op_short_fits_the_double_height_font() qui tient cette
 * contrainte, et elle seule. Allonger un libelle ici sans regarder ce test
 * tronquerait l'affichage en plein milieu d'un glyphe.
 *
 * Une valeur hors enum dit « INCONNU » et jamais le libelle d'une operation
 * reelle : sur l'ecran qui annonce ce qu'on autorise, afficher « SIGNATURE »
 * pour un code aberrant ferait confirmer autre chose que ce qui est montre.
 */
static inline const char *screen_op_short(sec_op_t op)
{
    switch (op) {
    case SEC_OP_SIGN:    return "SIGNATURE";
    case SEC_OP_DECRYPT: return "DECHIFFRER";
    case SEC_OP_AUTH:    return "AUTH";
    case SEC_OP_OTP:     return "CLE OTP";
    case SEC_OP_FIDO_REGISTER: return "CREER CLE";
    case SEC_OP_FIDO_AUTH:     return "AUTH FIDO";
    case SEC_OP_UNKNOWN: return "INCONNU";
    default:             return "INCONNU";
    }
}

/*
 * Logo errant de la veille : deux periodes, une par axe.
 *
 * Volontairement premieres entre elles et non multiples l'une de l'autre. Si les
 * deux axes avancaient du meme pas, le logo suivrait une diagonale et ne
 * visiterait qu'une bande de la dalle — la brulure se concentrerait sur cette
 * bande au lieu de se repartir, ce qui annulerait tout l'interet de le promener.
 * Avec 37 s et 23 s, le trajet ne se referme qu'au bout de 37 x 23 = 851 s.
 */
#define SCREEN_WANDER_X_MS 37000u
#define SCREEN_WANDER_Y_MS 23000u

/*
 * Coordonnee en dents de scie dans [0, span], de periode 2 x period_ms.
 *
 * Une dent de scie et non un modulo : un modulo ferait SAUTER le logo d'un bord
 * a l'autre a chaque cycle, ce qui se voit et ressemble a un defaut. La rampe
 * monte, atteint span a la demi-periode, et redescend.
 *
 * period_ms == 0 rend 0 sans diviser. Inatteignable avec les constantes
 * ci-dessus, et c'est justement pourquoi rien d'autre qu'un test ne le protege :
 * une division par zero sur un P4 ne leve pas d'exception, elle rend un
 * resultat indefini — donc un logo pose n'importe ou, y compris hors cadre.
 */
static inline uint16_t screen_wander(uint32_t now_ms, uint32_t period_ms, uint16_t span)
{
    if (period_ms == 0u) {
        return 0;
    }
    /* Pas de garde sur span == 0 : le produit ci-dessous rend deja 0, et une
     * mutation retirant une telle garde reste verte — signe qu'elle etait du
     * code mort plutot qu'une protection. test_wander_zero_span_is_immobile()
     * tient tout de meme le comportement, par le chemin arithmetique. */
    const uint32_t phase = now_ms % (2u * period_ms);
    const uint32_t up    = (phase < period_ms) ? phase : (2u * period_ms - phase);
    /* span <= 128 et up <= period_ms : le produit tient largement dans 32 bits
     * pour les periodes du projet (128 x 37000 = 4,7 M). */
    return (uint16_t)((up * (uint32_t)span) / period_ms);
}

/*
 * La dalle doit-elle etre franchement ETEINTE ?
 *
 * Deux seuils et non un. screen_blank_after_ms() fait entrer en veille — le logo
 * se promene, ce que Mae voulait voir plutot qu'un ecran noir. Celui-ci coupe
 * pour de bon bien plus tard : le logo errant repartit la brulure sur huit fois
 * plus de surface mais ne l'annule pas, et une cle reste branchee des annees.
 *
 * L'ordre entre les deux seuils est ce qui compte, et rien dans le compilateur
 * ne le garantit : si l'extinction arrivait la premiere, l'animation ne serait
 * jamais visible une seule image. C'est
 * test_sleep_comes_strictly_after_standby() qui tient cette relation.
 */
static inline bool screen_sleep_after_ms(uint32_t last_activity_ms, uint32_t now_ms)
{
    const uint32_t idle = now_ms - last_activity_ms;   /* juste au repassage a zero */
    return idle >= SCREEN_SLEEP_AFTER_MS;
}

/*
 * Course disponible pour un element mobile : ce qui reste de `total_px` une fois
 * `used_px` occupes, et jamais moins que zero.
 *
 * C'est la garde que render_standby() n'avait pas. Il soustrayait a la main, et
 * si le groupe depassait l'ecran la soustraction non signee se repliait sur des
 * dizaines de milliers : le logo serait parti se promener hors champ, defaisant
 * precisement la fonction anti-marquage pour laquelle la veille existe.
 *
 * Meme faute que screen_center_x() documente et evite depuis le debut. La revue
 * de branche l'a retrouvee a quelques lignes de la — signe qu'une regle
 * enoncee dans un commentaire ne se propage pas toute seule, et qu'il fallait
 * une fonction pour la porter.
 */
static inline uint16_t screen_span(uint16_t total_px, uint16_t used_px)
{
    if (used_px >= total_px) {
        return 0;
    }
    return (uint16_t)(total_px - used_px);
}
