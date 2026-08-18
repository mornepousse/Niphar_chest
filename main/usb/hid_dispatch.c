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

/*
 * I1 (revue finale de branche) — UNE SEULE lecture de s_h par callback, dans
 * un local NON volatile.
 *
 * `(s_h && s_h->champ) ? s_h->champ(...) : ...` fait TROIS accès distincts à
 * la variable volatile (le test de nullité, la lecture du champ, l'appel
 * lui-même) : chacun est un vrai chargement, confirmé au désassemblage. Si
 * hid_dispatch_set(NULL) s'intercale entre deux de ces chargements — MODE
 * pressé pendant qu'un navigateur sonde U2F toutes les ~117 ms, par exemple
 * — le dernier déréférence un pointeur devenu NULL : panique.
 *
 * La copie locale `h` fige la valeur pour tout le reste de la fonction : un
 * seul chargement, donc soit s_h était déjà NULL et rien n'est appelé, soit
 * il ne l'était pas et `h` reste valide pendant tout l'appel qui suit, quoi
 * que fasse hid_dispatch_set() entre-temps sur la variable partagée.
 *
 * Le volatile sur s_h RESTE nécessaire : il garantit la VISIBILITÉ entre
 * tâches (qu'une écriture de usb_mode.c finisse par être vue par tud_task).
 * La copie locale garantit la LECTURE UNIQUE. Les deux sont complémentaires,
 * pas redondants : retirer l'un ou l'autre rouvre exactement le trou que
 * cette correction ferme — ne pas « simplifier ».
 */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    const hid_handlers_t *h = s_h;
    return (h && h->report_desc) ? h->report_desc() : NULL;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    const hid_handlers_t *h = s_h;
    return (h && h->get_report)
         ? h->get_report(report_id, report_type, buffer, reqlen) : 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    const hid_handlers_t *h = s_h;
    if (h && h->set_report) {
        h->set_report(report_id, report_type, buffer, bufsize);
    }
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len)
{
    (void)instance;
    const hid_handlers_t *h = s_h;
    if (h && h->report_complete) {
        h->report_complete(report, len);
    }
}
