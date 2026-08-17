#include "usb/hid_dispatch.h"

/* hid_dispatch.c — propriétaire unique des callbacks HID de TinyUSB.
 *
 * TinyUSB les résout par symbole global : deux modes HID ne peuvent donc pas
 * les définir chacun. Ce fichier les possède et route vers le mode installé.
 * Aucun mode installé -> réponses inertes, jamais de déréférencement nul.
 *
 * s_h est écrit par la tâche appelante de hid_dispatch_set() (la bascule de
 * mode USB, dans usb/usb_mode.c) et lu par les callbacks tud_hid_*_cb
 * ci-dessous, appelés depuis tud_task — deux tâches FreeRTOS différentes.
 * Le pointeur est donc volatile, comme le sont les drapeaux inter-tâches
 * équivalents dans security/ccid.c (ex. ccid.c:196-223) : sans ce
 * qualificatif, rien dans le langage C ne garantit qu'une écriture faite par
 * une tâche devienne visible à l'autre.
 * C'est le POINTEUR qui est volatile, pas sa cible (hid_handlers_t reste
 * const) : `volatile const hid_handlers_t *` qualifierait la cible et pas le
 * pointeur, et laisserait le même trou. */
static const hid_handlers_t * volatile s_h = NULL;

void hid_dispatch_set(const hid_handlers_t *h) { s_h = h; }

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return (s_h && s_h->report_desc) ? s_h->report_desc() : NULL;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    return (s_h && s_h->get_report)
         ? s_h->get_report(report_id, report_type, buffer, reqlen) : 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    if (s_h && s_h->set_report) {
        s_h->set_report(report_id, report_type, buffer, bufsize);
    }
}
