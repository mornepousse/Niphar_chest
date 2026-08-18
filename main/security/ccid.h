/* main/security/ccid.h — minimal TinyUSB CCID class driver (dongle only, Phase 0)
 *
 * Carries APDUs between the host (scdaemon/gpg) and the OpenPGP applet
 * (openpgp_card.c) over a USB CCID smartcard interface.
 *
 * The driver is registered with TinyUSB via the weak usbd_app_driver_get_cb()
 * hook, which is implemented in ccid.c. TinyUSB picks it up at
 * tinyusb_driver_install() time and invokes the driver init() callback, which
 * wires the OpenPGP applet (openpgp_card_init).
 *
 * Compiled for the dongle role only (see main/CMakeLists.txt).
 */
#pragma once

#include <stdint.h>

/* sec_op_t — l'operation que ccid_confirm_named() fait annoncer a l'ecran. */
#include "sec_confirm.h"

/* ccid_init() — call once from force-linked USB init (kase_tinyusb_init).
 *
 * The weak usbd_app_driver_get_cb() override only takes effect if ccid.c.obj is
 * actually linked. Since the object exports no other referenced symbol, the
 * linker would otherwise drop it and the empty weak default in libtinyusb.a
 * would win — no CCID app driver registered, and SET_CONFIGURATION asserts
 * because no driver claims the CCID interface. Calling ccid_init() forces the
 * object into the link. See the comment on ccid_init() in ccid.c. Do not
 * remove the call. */
void ccid_init(void);

/*
 * ccid_shutdown() — à appeler AVANT tout démontage de TinyUSB, jamais après.
 *
 * Divergence propre au coffre (voir la divergence BLOQUANT 1 en tête de
 * ccid.c) : chez KeSp la pile USB est installée une fois pour toutes, ici elle
 * est démontée à chaque bascule de mode. La tâche ccid_worker, elle, est créée
 * une seule fois et survit à tout ; pendant l'attente de confirmation physique
 * elle poste un WTX toutes les 1,5 s sur la file de tud_task, que tud_deinit()
 * détruit. Cette fonction ferme cette porte, puis attend que le worker soit au
 * repos.
 *
 * Idempotente, et sans effet si ccid_init() n'a jamais tourné. Bloque au plus
 * ~2 s ; en pratique quelques dizaines de millisecondes. Une confirmation en
 * cours est REFUSÉE — aucun geste physique n'a eu lieu.
 */
void ccid_shutdown(void);

/* ------------------------------------------------------------------ */
/* Aiguillage d'applet — un seul transport CCID, deux applets           */
/* ------------------------------------------------------------------ */

/*
 * Traite une commande APDU et rend la longueur ecrite dans `out` (mot d'etat
 * compris). Meme contrat que openpgp_card_apdu().
 */
typedef uint16_t (*ccid_applet_fn_t)(const uint8_t *in, uint16_t in_len,
                                     uint8_t *out, uint16_t out_max);

/*
 * Choisit qui repond aux XfrBlock. NULL retablit l'applet OpenPGP, qui reste
 * le DEFAUT : c'est le chemin valide sur materiel (docs/HARDWARE.md), et il
 * doit rester celui qu'on obtient quand personne n'a rien demande.
 *
 * A appeler par le mode USB, apres un usb_device_install() reussi et avant
 * de laisser l'hote parler. ccid_drv_init() le remet a NULL a chaque
 * installation de pile : un applet oublie par un mode qui s'en va ne peut
 * donc pas repondre a la place du suivant.
 *
 * Un seul applet a la fois, comme un seul mode USB a la fois : le worker CCID
 * est unique et serialise les XfrBlock (bMaxCCIDBusySlots=1), donc il n'y a
 * jamais deux commandes en vol pour deux applets differents.
 */
void ccid_set_applet(ccid_applet_fn_t applet);

/*
 * Attend l'appui physique, en nommant a l'ecran l'operation ET le compte
 * concerne. Rend 1 si l'appui a ete donne dans la fenetre, 2 sinon (refus,
 * expiration, ou demontage de la pile USB en cours).
 *
 * `label` peut etre NULL quand l'operation n'a pas de compte a nommer ; il
 * est assaini et tronque par sec_confirm_arm_named().
 *
 * BLOQUE l'appelant — donc le worker CCID, donc l'hote — jusqu'a
 * SEC_CONFIRM_TIMEOUT_MS. Ce n'est pas un defaut : c'est ce blocage qui fait
 * qu'aucune autre commande ne peut modifier le magasin entre la demande et
 * l'appui (voir la note DEPENDANCE AU TRANSPORT de security/oath_proto.h).
 * L'hote est tenu au courant par des trames WTX toutes les 1,5 s.
 */
int ccid_confirm_named(sec_op_t op, const char *label);
