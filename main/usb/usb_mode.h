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
    USB_MODE_COUNT,
} usb_mode_t;

/* Démarre en USB_MODE_NONE, sans toucher au matériel : rien n'est encore
 * installé, donc il n'y a rien à désinstaller. */
esp_err_t usb_mode_init(void);

/*
 * Bascule vers `mode`, avec ré-énumération côté hôte. Si `mode` est déjà le
 * mode courant, ne fait rien et renvoie ESP_OK. USB_MODE_PGP et USB_MODE_OTP
 * renvoient ESP_ERR_NOT_SUPPORTED tant que leurs descripteurs n'existent pas
 * (tâches 10 et 11).
 */
esp_err_t usb_mode_set(usb_mode_t mode);

usb_mode_t usb_mode_get(void);

/* Jamais NULL, même pour une valeur hors bornes : cette fonction doit rester
 * utilisable pour journaliser une bascule invalide. */
const char *usb_mode_name(usb_mode_t mode);
