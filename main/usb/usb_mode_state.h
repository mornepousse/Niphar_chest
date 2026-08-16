#pragma once

/*
 * usb_mode_state — que doit annoncer le coffre quand une bascule de mode
 * échoue en cours de route ?
 *
 * Seul morceau de logique PURE de usb_mode.c, sorti ici pour être testable sur
 * l'hôte (test/test_usb_mode.c), sur le modèle exact de security/ccid_zlp.h :
 * usb_mode.c lui-même ne compile qu'avec ESP-IDF et TinyUSB.
 *
 * Pourquoi un fichier pour deux lignes : c'est la décision qui s'est trompée
 * DEUX fois, toujours dans le même sens — annoncer USB_MODE_NONE alors que
 * l'hôte voit encore un périphérique. La règle tient en une phrase :
 *
 *     ce que dit s_mode doit décrire ce que voit L'HÔTE, pas l'intention
 *     de l'appelant.
 *
 * Deux endroits d'échec, deux vérités différentes :
 *
 *  - USB_MODE_FAIL_UNINSTALL — usb_device_uninstall() a rendu une erreur. Dans
 *    ce cas il n'a RIEN détaché : ni tud_disconnect(), ni tusb_deinit(), ni le
 *    PHY (voir le contrat en tête de usb_device.h). Le périphérique du mode
 *    précédent est donc toujours sur le bus, et c'est lui qu'il faut annoncer.
 *    Cette conservation n'est pas cosmétique : c'est elle qui fait repasser la
 *    tentative suivante par mode_pgp_stop(), donc referme la porte des
 *    callbacks CCID avant le tusb_deinit() qui détruit la file de tud_task
 *    (divergence BLOQUANT 1 en tête de security/ccid.c).
 *
 *  - USB_MODE_FAIL_AFTER_UNINSTALL — la désinstallation a réussi, c'est la
 *    suite qui a échoué (init du disque, installation des descripteurs). Plus
 *    rien n'est exposé : USB_MODE_NONE est la vérité.
 *
 * Dans les deux cas `known` est faux. C'est ce qui empêche le court-circuit
 * « mode déjà courant » du sélecteur de renvoyer ESP_OK sans rien faire, et
 * donc ce qui rend la reprise possible : redemander le même mode doit retenter
 * la séquence complète.
 *
 * (Le nom du sélecteur n'est pas écrit ici : le garde-fou 4 de scripts/fast.sh
 * interdit ce symbole hors de usb_mode.{c,h} et console.c, y compris en
 * commentaire. C'est voulu — un grep qui accepterait les commentaires ne
 * protégerait plus rien.)
 */

#include <stdbool.h>

#include "usb/usb_mode.h"

typedef enum {
    /* usb_device_uninstall() a échoué — rien n'a été détaché. */
    USB_MODE_FAIL_UNINSTALL = 0,
    /* Elle a réussi, mais une étape ultérieure a échoué. */
    USB_MODE_FAIL_AFTER_UNINSTALL,
} usb_mode_fail_t;

typedef struct {
    usb_mode_t mode;    /* ce que usb_mode_get() doit rendre */
    bool       known;   /* cet état est-il certain ? */
} usb_mode_state_t;

static inline usb_mode_state_t usb_mode_state_on_failure(usb_mode_t previous,
                                                         usb_mode_fail_t where)
{
    usb_mode_state_t st;
    /* Jamais certain : une bascule qui échoue laisse le matériel dans un état
     * qu'on n'a pas fini de commander. */
    st.known = false;
    st.mode  = (where == USB_MODE_FAIL_UNINSTALL) ? previous : USB_MODE_NONE;
    return st;
}
