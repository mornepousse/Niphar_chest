#pragma once

/*
 * hmi — les deux boutons et la LED de la carte-cle.
 *
 * Ce module ne contient AUCUNE decision : elles sont toutes dans les trois
 * en-tetes purs qu'il consomme (hmi/button_debounce.h, hmi/led_state.h,
 * usb/usb_mode_cycle.h), pour rester testables sur l'hote. Ici, il n'y a que
 * du GPIO, du RMT et une tache.
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
