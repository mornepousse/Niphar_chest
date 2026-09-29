#pragma once

/*
 * usb_mode — une fonction USB à la fois.
 *
 * Le coffre est commandé par le clavier et n'expose jamais deux fonctions
 * ensemble : un disque et une clé PGP sur le même bus en même temps serait
 * une clé exposée « parce qu'on voulait un disque ». C'est pourquoi
 * USB_MODE_NONE vaut zéro — l'état d'une variable statique non initialisée —
 * et c'est l'état de démarrage : tant que rien ne l'a demandé explicitement,
 * le coffre n'expose rien du tout.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef TEST_HOST
/* usb_mode_name.c est compilé sur l'hôte (voir test/CMakeLists.txt) : pas
 * d'ESP-IDF disponible, donc pas de vrai esp_err.h. Même faux typedef que
 * les autres en-têtes portés compilés côté hôte (cf. TEST_HOST ailleurs dans
 * main/security/). */
typedef int esp_err_t;
#else
#include "esp_err.h"
#endif

typedef enum {
    USB_MODE_NONE = 0,   /* aucune fonction exposée — état au démarrage */
    USB_MODE_STORAGE,    /* le disque (MSC) */
    USB_MODE_PGP,        /* la carte OpenPGP (CCID) — tâche 10 */
    USB_MODE_OTP,        /* la clé CR-HMAC (HID) — tâche 11 */
    USB_MODE_FIDO,       /* l'authentificateur U2F/CTAP-HID (HID) — plan FIDO2, tâche 3 */
    USB_MODE_OATH,       /* les comptes TOTP YKOATH (CCID) — plan OATH, tâche 6 */
    USB_MODE_COUNT,
} usb_mode_t;

/* Démarre en USB_MODE_NONE, sans toucher au matériel : rien n'est encore
 * installé, donc il n'y a rien à désinstaller. */
esp_err_t usb_mode_init(void);

/*
 * Bascule vers `mode`, avec ré-énumération côté hôte. Si `mode` est déjà le
 * mode courant, ne fait rien et renvoie ESP_OK. NONE, STORAGE, PGP et OTP
 * sont branchés depuis la tâche 11 ; FIDO depuis la tâche 3 du plan FIDO2.
 *
 * Peut être appelée depuis deux tâches sur wt9932_key (bouton MODE et
 * console). Ce n'est PAS réentrant en interne — voir le commentaire de
 * s_busy dans le .c — donc un appel qui arrive pendant qu'une bascule est
 * déjà en cours rend immédiatement ESP_ERR_INVALID_STATE, sans attendre :
 * jamais d'attente bloquante ici, une bascule peut durer jusqu'à 15 s.
 */
esp_err_t usb_mode_set(usb_mode_t mode);

/*
 * Passe au mode suivant du cycle de la carte-clé (usb/usb_mode_cycle.h).
 *
 * Existe pour que l'IHM n'ait pas à appeler usb_mode_set : ce symbole est
 * confiné par le garde-fou 4 de scripts/fast.sh à ce module et à la console,
 * et l'élargir à hmi.c affaiblirait le garde pour un gain nul. La politique du
 * cycle reste chez le module qui possède les modes.
 */
esp_err_t usb_mode_cycle_next(void);

/*
 * Applique un mode demandé par le LIEN S3↔coffre, désigné par sa valeur de fil
 * (link/link_proto.h, LINK_USB_MODE_*) et non par usb_mode_t.
 *
 * EXISTE POUR LA MÊME RAISON QUE usb_mode_cycle_next(), et c'est la seule
 * raison. usb_mode_set est confiné par le garde-fou 4 de scripts/fast.sh à ce
 * module et à la console ; le lien doit pourtant pouvoir changer de mode, parce
 * que sur le coffre il est le SEUL à le pouvoir (boards/niphar_chest/board.h
 * pose BOARD_CONSOLE_ACTIONS 0, donc la béquille console n'y est même pas
 * compilée — sans ce chemin, le coffre démarre en USB_MODE_NONE et rien ne l'en
 * sort). Élargir la liste des fichiers autorisés aurait rendu le garde-fou plus
 * permissif pour tout le monde afin de servir un seul appelant ; un point
 * d'entrée nommé le sert sans rien relâcher, et la politique — quelle valeur de
 * fil correspond à quel mode, et laquelle se refuse — reste chez le module qui
 * possède les modes.
 *
 * Une valeur de fil non attribuée est REFUSÉE : ESP_ERR_INVALID_ARG, le mode
 * courant est conservé, et le refus est journalisé. Jamais de repli sur
 * USB_MODE_NONE — ça démonterait l'interface que la propriétaire est en train
 * d'utiliser en réponse à un octet qu'on n'a pas compris.
 *
 * Sinon, rend ce que rend usb_mode_set() pour le mode correspondant, y compris
 * ESP_ERR_INVALID_STATE quand une bascule est déjà en cours. L'appelant est
 * censé réessayer : voir link/link_spi.c, qui ne retient la valeur appliquée
 * qu'en cas de succès.
 */
esp_err_t usb_mode_apply_wire(uint8_t wire);

usb_mode_t usb_mode_get(void);

/*
 * Le mode rendu par usb_mode_get() est-il certain ?
 *
 * Faux entre le démontage de la pile et la réussite de l'installation
 * suivante, donc notamment après une bascule qui a échoué : ce que voit l'hôte
 * n'est alors plus garanti conforme. Sans cet accesseur, l'incertitude
 * n'existerait que dans une statique invisible, et la console afficherait un
 * mode d'aplomb dans le cas précis où il ne faut pas s'y fier.
 */
bool usb_mode_is_known(void);

/* Jamais NULL, même pour une valeur hors bornes : cette fonction doit rester
 * utilisable pour journaliser une bascule invalide. */
const char *usb_mode_name(usb_mode_t mode);
