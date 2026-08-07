#pragma once

/*
 * Descripteur fonctionnel CCID et macro d'interface (USB CCID Rev 1.1 §5.1).
 * TinyUSB n'a pas de macro pour cette classe — pas assez répandue pour être
 * du noyau — donc KeSp l'a écrite à la main.
 *
 * Copié verbatim depuis
 * KeSp_firmware/main/comm/usb/usb_hid.c:120-162 (KASE_CCID_DESC,
 * TUD_CCID_DESC_LEN, KASE_CCID_ITF_DESC). Le préfixe KASE_ est conservé tel
 * quel : le renommer casserait la comparaison avec l'amont sans rien
 * apporter (voir .superpowers/sdd/2026-08-07-portage-ccid-otp/task-10-brief.md).
 *
 * DIVERGENCE (Niphar_chest tâche 12) : KASE_CCID_ITF_DESC gagne un 5ᵉ
 * paramètre `_epsize`, absent de l'original. L'amont fige wMaxPacketSize à
 * 64 pour les deux points bulk, ce que mode_pgp.c reprenait tel quel pour
 * s_fs_config ET s_hs_config — mais l'USB 2.0 (tableau 5-5) impose 512 comme
 * SEULE valeur légale pour un point bulk en haute vitesse, jamais 64. Sur le
 * coffre, qui négocie la haute vitesse (contrairement au dongle KeSp,
 * jamais testé à cette vitesse pour CCID), ce descripteur non conforme
 * faisait échouer le pilote CCID interne de scdaemon dès l'ouverture
 * (« unexpected bulk-in msg type (00) » puis timeout) — trouvé en validant
 * `gpg --card-status` sur matériel. mode_storage.c fait déjà cette
 * distinction pour TUD_MSC_DESCRIPTOR (64 en FS, 512 en HS) ; CCID suit
 * maintenant le même modèle. Voir l'appel dans mode_pgp.c.
 */

#include "tusb.h"

/* CCID functional descriptor (USB CCID Rev 1.1 §5.1) — single slot, T=1, short+extended APDU.
 * Cross-check every field against the spec and a real reader (e.g. a YubiKey) before trusting. */
#define KASE_CCID_DESC \
    0x36, 0x21,                 /* bLength=54, bDescriptorType=0x21 (CCID) */ \
    0x10, 0x01,                 /* bcdCCID = 1.10 */ \
    0x00,                       /* bMaxSlotIndex = 0 (one slot) */ \
    0x07,                       /* bVoltageSupport = 5.0/3.0/1.8 */ \
    0x02, 0x00, 0x00, 0x00,     /* dwProtocols = T=1 */ \
    0xA0, 0x0F, 0x00, 0x00,     /* dwDefaultClock = 4000 kHz */ \
    0xA0, 0x0F, 0x00, 0x00,     /* dwMaximumClock = 4000 kHz */ \
    0x00,                       /* bNumClockSupported = 0 (only default) */ \
    0x80, 0x25, 0x00, 0x00,     /* dwDataRate = 9600 */ \
    0x80, 0x25, 0x00, 0x00,     /* dwMaxDataRate = 9600 */ \
    0x00,                       /* bNumDataRatesSupported = 0 */ \
    0xFE, 0x00, 0x00, 0x00,     /* dwMaxIFSD = 254 */ \
    0x00, 0x00, 0x00, 0x00,     /* dwSynchProtocols = 0 */ \
    0x00, 0x00, 0x00, 0x00,     /* dwMechanical = 0 */ \
    0x40, 0x08, 0x04, 0x00,     /* dwFeatures = 0x00040840 (LE): 0x00000040 automatic
                                 * parameters negotiation + 0x00040000 short+extended
                                 * APDU level exchange (the bit scdaemon requires). This
                                 * is the field scdaemon is pickiest about and the #1
                                 * de-risk knob: if gpg --card-status does not enumerate
                                 * the reader, tune this first — cross-check vs a real
                                 * YubiKey (lsusb -v) and USB CCID Rev 1.1 §5.1. */ \
    0x0F, 0x01, 0x00, 0x00,     /* dwMaxCCIDMessageLength = 271 (short) — raise for extended */ \
    0xFF,                       /* bClassGetResponse = echo */ \
    0xFF,                       /* bClassEnvelope = echo */ \
    0x00, 0x00,                 /* wLcdLayout = none */ \
    0x00,                       /* bPINSupport = 0 (no pinpad; PIN over APDU) */ \
    0x01                        /* bMaxCCIDBusySlots = 1 */

/* Interface: CCID smartcard reader (class 0x0B). No TinyUSB macro exists.
 * `_epsize` : 64 pour un descripteur plein débit, 512 pour haute vitesse —
 * voir la divergence documentée en tête de fichier. */
#define TUD_CCID_DESC_LEN (9 + 54 + 7 + 7)   /* itf + CCID class desc + 2 EP */
#define KASE_CCID_ITF_DESC(_itfnum, _stridx, _epout, _epin, _epsize) \
    /* Interface: bNumEndpoints=2, class 0x0B (CCID), subclass 0x00, \
     * bInterfaceProtocol=0x00 (bulk; 0x01/0x02 would be ICCD) */ \
    9, TUSB_DESC_INTERFACE, _itfnum, 0, 2, TUSB_CLASS_SMART_CARD, 0x00, 0x00, _stridx, \
    /* CCID functional/class descriptor (54 bytes) */ \
    KASE_CCID_DESC, \
    /* Bulk OUT endpoint */ \
    7, TUSB_DESC_ENDPOINT, _epout, TUSB_XFER_BULK, U16_TO_U8S_LE(_epsize), 0, \
    /* Bulk IN endpoint */ \
    7, TUSB_DESC_ENDPOINT, _epin, TUSB_XFER_BULK, U16_TO_U8S_LE(_epsize), 0
