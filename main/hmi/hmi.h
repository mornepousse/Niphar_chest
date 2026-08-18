#pragma once

/*
 * hmi — les deux boutons et la LED de la carte-cle.
 *
 * Ce module N'EST PAS sans decision : cette affirmation a longtemps figure
 * ici, et elle etait fausse — une revue de branche l'a releve. Ce qui est
 * pur et teste sur l'hote (hmi/button_debounce.h, hmi/led_state.h,
 * usb/usb_mode_cycle.h) couvre l'anti-rebond, le mapping etat -> couleur, et
 * l'ordre du cycle de modes. Mais hmi.c lui-meme decide encore :
 *
 * - QUAND une attente s'arme (transition pending false -> true) : hmi.c
 *   memorise cet instant et le passe a hmi/led_state.h:led_wait_phase(),
 *   qui elle est pure et testee, et qui decide seule la periode et la phase
 *   de l'alternance a partir de cet instant ;
 * - la duree du flash de verdict (HMI_FLASH_MS) et l'arithmetique de son
 *   echeance (event_until, comparee avec un soustraction uint32_t signee) ;
 * - la priorite entre evenements quand plusieurs sont vrais au meme tick
 *   (un flash en cours masque l'alternance, une bascule de mode ecrase un
 *   flash de verdict en attente) ;
 * - la regle « ne consommer un appui CONFIRM, et ne flasher, que si une
 *   operation est armee » (peek() avant d'agir, jamais sur la seule lecture
 *   du bouton).
 *
 * Rien de tout ca n'est teste depuis ce fichier : c'est du code materiel,
 * qui ne compile pas sur l'hote. Un futur lecteur qui cherche un bug de
 * timing ou de priorite doit le chercher ICI, pas se laisser dire qu'il n'y
 * a rien a y trouver.
 *
 * Depuis la tache ecran (hmi/screen.c) : hmi.c publie aussi, en fin de
 * chaque tick, un hmi_snapshot_t sous verrou — mode, attente, operation et
 * evenement LED, copies d'un coup. Ce n'est pas une decision au sens des
 * puces ci-dessus (rien n'y est choisi, tout y est deja calcule), mais c'est
 * une responsabilite de plus que ce fichier porte, et une ou une lecture
 * dechiree romprait la meme garantie que sec_confirm_peek_labeled() protege
 * cote securite : voir hmi_snapshot_t plus bas.
 *
 * N'existe que sur une carte qui a un bouton — voir boards/wt9932_key/board.h.
 */

#include "esp_err.h"

#include "hmi/led_state.h"
#include "sec_confirm.h"
#include "usb/usb_mode.h"
/* Frere sans prefixe, comme sec_confirm.h ci-dessus (main/security/ est a
 * plat sur l'include path). Seul OATH_NAME_DISPLAY_MAX est utilise : la
 * taille de `label` ci-dessous doit rester le meme nombre que le tampon
 * source dans sec_confirm.c, sans quoi une copie tronquerait ou deborderait. */
#include "oath_name.h"

/*
 * Ce que la tache d'affichage a besoin de savoir. Publie par la tache IHM sous
 * verrou, lu par la tache ecran : deux contextes, donc un instantane copie d'un
 * coup plutot que quatre champs lus separement — une lecture dechiree
 * afficherait une operation avec l'echeance d'une autre.
 *
 * Le verrou ne protege QUE la copie de cette structure. Il n'est jamais tenu
 * pendant un transfert I2C.
 */
typedef struct {
    usb_mode_t  mode;
    bool        confirm_pending;
    uint32_t    armed_at_ms;
    sec_op_t    op;
    /* Etiquette de l'operation OATH armee (compte, ou compte a la forme
     * "N COMPTES" pour un RESET) — vide pour toute operation qui n'en porte
     * pas. Un tableau de taille fixe, pas un `const char *` : copie par
     * valeur avec le reste de l'instantane sous s_snap_lock, jamais un
     * pointeur vers l'etat mutable de sec_confirm.c qui resterait valide
     * apres le verrou relache. */
    char        label[OATH_NAME_DISPLAY_MAX];
    led_event_t event;
    uint32_t    event_at_ms;
} hmi_snapshot_t;

/*
 * Configure les deux GPIO d'entree (pull-up interne, actifs bas), le pilote
 * led_strip, et demarre la tache de scrutation.
 *
 * A appeler APRES usb_mode_init() : le premier appui MODE bascule le mode, et
 * le selecteur doit exister. Une erreur ici n'est pas fatale — la cle reste
 * pilotable par la console.
 */
esp_err_t hmi_init(void);

/*
 * Copie l'instantane courant dans *out, sous verrou. Sans effet sur l'etat de
 * sec_confirm : hmi_task l'a deja lu via sec_confirm_peek_labeled() avant de
 * publier. Sur une carte sans bouton (hmi_init() no-op), *out reste a sa
 * valeur zero-initialisee — mode NONE, rien en attente, aucun evenement — ce
 * qui est le bon defaut pour screen.c sur les cartes qui n'ont pas d'ecran
 * non plus.
 */
void hmi_snapshot(hmi_snapshot_t *out);
