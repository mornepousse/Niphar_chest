#pragma once

/*
 * Configuration TinyUSB du coffre.
 *
 * Ce fichier remplace celui d'espressif/esp_tinyusb : on pilote TinyUSB
 * directement pour garder la main sur les callbacks MSC (voir
 * main/idf_component.yml pour le pourquoi).
 *
 * Il est rendu visible au composant tinyusb depuis main/CMakeLists.txt.
 */

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Contrôleur                                                                */
/* ------------------------------------------------------------------------ */

#define CFG_TUSB_OS                 OPT_OS_FREERTOS
#define CFG_TUD_ENABLED             1
#define CFG_TUH_ENABLED             0

/*
 * Le coffre expose son disque sur l'OTG 2.0 haute vitesse (PHY UTMI), rhport 1.
 * L'USB-Serial-JTAG, lui, est un contrôleur distinct : il n'apparaît pas ici et
 * n'est pas concerné par cette configuration.
 */
#define CFG_TUD_MAX_SPEED           OPT_MODE_HIGH_SPEED

/*
 * Le rhport 0 est l'OTG 1.1 pleine vitesse, le rhport 1 l'OTG 2.0 haute
 * vitesse. Déclarer le mode ici est ce qui permet à tusb_init() sans argument
 * de savoir quoi initialiser.
 */
#define CFG_TUSB_RHPORT0_MODE       OPT_MODE_NONE
#define CFG_TUSB_RHPORT1_MODE       (OPT_MODE_DEVICE | OPT_MODE_HIGH_SPEED)

/* DMA du DWC2 : le P4 a un cache, TinyUSB doit le prendre en compte. */
#define CFG_TUD_DWC2_DMA_ENABLE     1
#define CFG_TUD_DWC2_SLAVE_ENABLE   1
#define CFG_TUD_MEM_DCACHE_ENABLE   1
#define CFG_TUD_MEM_DCACHE_LINE_SIZE CONFIG_CACHE_L1_CACHE_LINE_SIZE

/* Les tampons remis au DMA doivent être alignés sur une ligne de cache. */
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN          __attribute__((aligned(CONFIG_CACHE_L1_CACHE_LINE_SIZE)))

#define CFG_TUD_ENDPOINT0_SIZE      64

/* ------------------------------------------------------------------------ */
/* Classes                                                                   */
/* ------------------------------------------------------------------------ */

/*
 * MSC ici. Le CCID (OpenPGP, tâche 10) n'a rien à y activer : ce n'est pas
 * une classe standard de TinyUSB mais un pilote applicatif (security/ccid.c,
 * enregistré via usbd_app_driver_get_cb). HID (OTP, tâche 11), en revanche,
 * *est* une classe standard de TinyUSB — contrairement au commentaire qui
 * était ici avant cette tâche, il ne suit pas le chemin du CCID : pas de
 * pilote applicatif à force-linker, TinyUSB appelle directement les
 * callbacks tud_hid_* (usb/mode_otp.c) une fois CFG_TUD_HID à 1. usb_mode.h
 * garde MSC, CCID et HID mutuellement exclusifs, ce qui évite le budget
 * d'endpoints IN qui a forcé KeSp_firmware à faire pareil sur ses
 * interfaces de sécurité.
 */
#define CFG_TUD_MSC                 1

/*
 * Taille du tampon que TinyUSB remplit par appel à tud_msc_read10_cb().
 *
 * C'est le paramètre de débit du MSC : chaque appel est une transaction SDMMC
 * synchrone, sans recouvrement avec l'USB, donc plus les blocs sont gros moins
 * la latence pèse. Mesuré sur le kit avec la SE04G : 4 Kio donnaient 5,8 Mio/s
 * en lecture là où le SDMMC brut fait 18,3 Mio/s par blocs de 64 Kio
 * (« sd bench » sur la console).
 *
 * 32 Kio = 64 secteurs par transfert. Le coût est un tampon statique de 32 Kio
 * en RAM interne accessible au DMA — supportable, et la RAM interne est
 * justement ce qui reste abondant ici puisque le gros vit en PSRAM.
 */
#define CFG_TUD_MSC_EP_BUFSIZE      32768

#define CFG_TUD_CDC                 0

/*
 * HID porte le mode OTP (tâche 11) : la clé CR-HMAC, protocole Yubikey OTP
 * (feature reports de 8 octets). CFG_TUD_HID_EP_BUFSIZE dimensionne le
 * tampon de l'endpoint interrupt IN de la classe — jamais utilisée en
 * pratique ici (voir usb/mode_otp.c), 64 octets est la valeur plancher
 * confortable de TinyUSB, pas un budget mesuré comme pour le MSC ci-dessus.
 */
#define CFG_TUD_HID                 1
#define CFG_TUD_HID_EP_BUFSIZE      64

#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0
#define CFG_TUD_AUDIO               0
#define CFG_TUD_VIDEO               0
#define CFG_TUD_DFU                 0
#define CFG_TUD_DFU_RUNTIME         0
#define CFG_TUD_ECM_RNDIS           0
#define CFG_TUD_NCM                 0
#define CFG_TUD_BTH                 0
#define CFG_TUD_USBTMC              0
#define CFG_TUD_PRINTER             0
#define CFG_TUD_MTP                 0

#ifdef __cplusplus
}
#endif
