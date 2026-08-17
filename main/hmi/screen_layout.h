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

/* Nombre de points de cycle dessines. Les quatre modes que le firmware
 * connait, dans l'ordre de usb_mode_t. */
static inline uint8_t screen_mode_count(void)
{
    return 4u;
}

/*
 * Quel point est plein, de 0 a screen_mode_count() - 1.
 *
 * Une valeur hors enum rend screen_mode_count() : AUCUN point ne s'allume.
 * Replier sur 0 ferait passer l'aberration pour « rien expose » — meme
 * principe que screen_mode_name(), qui rend « MODE INCONNU » plutot que le nom
 * d'un mode connu : un silence identique a un etat connu masque l'erreur au
 * lieu de la signaler.
 */
static inline uint8_t screen_mode_index(usb_mode_t mode)
{
    switch (mode) {
    case USB_MODE_NONE:    return 0u;
    case USB_MODE_STORAGE: return 1u;
    case USB_MODE_PGP:     return 2u;
    case USB_MODE_OTP:     return 3u;
    default:               return screen_mode_count();
    }
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
    case SEC_OP_UNKNOWN: return "INCONNU";
    default:             return "INCONNU";
    }
}
