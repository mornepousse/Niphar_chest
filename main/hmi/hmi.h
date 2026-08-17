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
 * N'existe que sur une carte qui a un bouton — voir boards/wt9932_key/board.h.
 */

#include "esp_err.h"

/*
 * Configure les deux GPIO d'entree (pull-up interne, actifs bas), le pilote
 * led_strip, et demarre la tache de scrutation.
 *
 * A appeler APRES usb_mode_init() : le premier appui MODE bascule le mode, et
 * le selecteur doit exister. Une erreur ici n'est pas fatale — la cle reste
 * pilotable par la console.
 */
esp_err_t hmi_init(void);
