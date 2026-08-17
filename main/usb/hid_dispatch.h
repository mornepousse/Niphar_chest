#pragma once

/*
 * Répartiteur des callbacks HID de TinyUSB — propriétaire unique.
 *
 * TinyUSB résout tud_hid_descriptor_report_cb(), tud_hid_get_report_cb() et
 * tud_hid_set_report_cb() par symbole global : un seul fichier peut donc les
 * définir dans tout le lien. mode_otp.c les possédait jusqu'ici ; dès qu'un
 * second mode HID (FIDO2/CTAP-HID) doit exister, il lui faut ce répartiteur
 * unique, qui route vers le mode HID actuellement installé.
 *
 * Un seul mode HID est actif à la fois — les modes USB du coffre sont
 * mutuellement exclusifs au run-time (voir usb/usb_mode.c). hid_dispatch_set()
 * pose donc un pointeur simple, pas une pile ni un registre indexé.
 */

#include <stdint.h>

#include "class/hid/hid.h"

/*
 * Table de handlers d'un mode HID. Chaque champ peut être NULL : le
 * répartiteur rend alors une réponse inerte (0, ou aucune action) plutôt que
 * de déréférencer un pointeur nul.
 */
typedef struct {
    const uint8_t *(*report_desc)(void);
    uint16_t (*get_report)(uint8_t report_id, hid_report_type_t type,
                           uint8_t *buffer, uint16_t reqlen);
    void     (*set_report)(uint8_t report_id, hid_report_type_t type,
                           const uint8_t *buffer, uint16_t bufsize);
} hid_handlers_t;

/*
 * Installe (ou retire, avec NULL) les handlers du mode HID actif. À appeler
 * par le mode lui-même : au succès de son installation USB pour poser sa
 * table, juste avant sa désinstallation pour la retirer (NULL) — sur le
 * modèle de mode_pgp_stop() côté CCID.
 */
void hid_dispatch_set(const hid_handlers_t *h);
