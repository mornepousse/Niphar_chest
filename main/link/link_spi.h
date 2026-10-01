#pragma once

/*
 * Transport du lien S3↔coffre — SPI2 en ESCLAVE, `spi_slave_hd`.
 *
 * Ce module ne décide de rien. Il installe le périphérique sur le brochage de
 * board.h, publie le bloc de registres que link/link_proto.c sérialise, relit
 * l'octet de confirmation que le maître y écrit, et pilote la ligne IRQ. Toute
 * la logique qui peut être fausse sans être visible — carte des registres, CRC,
 * détection d'absence, comparaison de version — vit dans link_proto, pur et
 * testé sur l'hôte. Rien de tout ça ne doit redescendre ici.
 *
 * Conception : docs/superpowers/specs/2026-08-07-lien-s3-coffre-design.md
 *
 * CE QUI PÈSE PLUS QUE LE RESTE. Le bus SCK/MOSI/MISO n'appartient pas au
 * coffre : c'est celui de la moitié gauche du clavier, partagé avec la nRF24
 * (et, côté droit, l'écran Sharp). Un esclave qui garde MISO en sortie hors
 * sélection tient le bus et rend la radio muette — même en fonctionnant
 * parfaitement. Ce n'est pas une hypothèse : un P4 non programmé l'a fait le
 * 2026-09-05, une heure de diagnostic, consigné dans docs/HARDWARE.md
 * (« Lien S3↔coffre — le brochage existe au PCB, pas dans la doc du clavier »).
 * D'où le contrat de ce module :
 *
 *   - les trois lignes partagées ne sont JAMAIS configurées en sortie ici, à
 *     aucun moment, même transitoirement ; seul le pilote SPI esclave y touche,
 *     et il ne les active que sous CS (« is only active on the bus when the
 *     Host asserts the Device's individual CS line », ESP-IDF
 *     docs/en/api-reference/peripherals/spi_slave.rst:24) ;
 *   - link_spi_init() s'appelle le PLUS TÔT possible dans app_main(), avant la
 *     microSD et l'USB : avant elle, ces broches ne sont que des GPIO, et cette
 *     fenêtre est le seul moment où le coffre n'a rien promis au clavier ;
 *   - un échec d'init laisse les cinq broches en ENTRÉE sans pull, et le dit au
 *     journal. Un échec qui laisserait des sorties actives serait pire que pas
 *     de lien du tout : le coffre survit sans lien, le clavier ne survit pas à
 *     un bus cloué.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

/*
 * Installe le transport. À appeler tôt — voir ci-dessus.
 *
 * Rend ESP_ERR_NOT_SUPPORTED sur une carte sans lien (BOARD_LINK_AVAILABLE 0),
 * ce qui n'est pas une panne : le kit et la carte-clé n'ont pas ce câblage.
 * Idempotente : un second appel sur un lien déjà installé rend ESP_OK.
 *
 * Un échec rend le code du pilote, après avoir remis les broches en entrée.
 */
esp_err_t link_spi_init(void);

/* Le transport est-il installé ? Faux avant link_spi_init(), après son échec,
 * et toujours sur une carte sans lien. */
bool link_spi_is_up(void);

/*
 * Bascule le bit LINK_STATE_READY du bloc de registres.
 *
 * « Prêt » veut dire une chose précise et vérifiable : app_main() est allé au
 * bout de son démarrage. Ce n'est pas la même information que « le lien
 * répond » — le bloc est publié bien avant, justement pour que le clavier
 * puisse voir un coffre en cours de démarrage plutôt que rien du tout.
 *
 * Sans effet sur une carte sans lien.
 */
void link_spi_set_ready(bool ready);

/*
 * Recopie le dernier bloc PUBLIE par le coffre (LINK_REG_CHEST_LEN octets),
 * pour la console. Rend false si rien n'a encore ete publie, ou si `cap` ne
 * suffit pas — sans toucher `out`.
 *
 * DIAGNOSTIC SEUL, et c'est sa raison d'etre. Sans maitre SPI en face, ce que
 * le coffre publie est INVISIBLE : on ne peut ni le lire, ni donc constater
 * qu'il ment. C'est exactement la ou le nombre de comptes d'un RESET a pu
 * valoir 1 pendant des heures alors que le contrat annoncait N — un defaut que
 * ni les tests hote ni le materiel ne pouvaient montrer, faute d'un endroit ou
 * regarder.
 *
 * En lecture seule, donc hors de BOARD_CONSOLE_ACTIONS : regarder n'est pas
 * agir, et la console du coffre garde son absence de pouvoir.
 */
bool link_spi_snapshot(uint8_t *out, size_t cap);

/*
 * Ce que le TRANSPORT sait de lui-meme, pour la console. Aucun de ces champs ne
 * traverse le fil : ils repondent aux questions qu'on se pose quand le fil ne
 * marche pas.
 *
 * `rx_armee` est la plus importante. Sans reception en file, un WRDMA du maitre
 * est perdu SANS ERREUR des deux cotes — c'est la panne muette que le contrat
 * impose d'ecarter, et elle s'est produite au premier demarrage de la v3 pour
 * un probleme d'alignement. La seule trace etait une ligne de journal au
 * demarrage, qui a defile depuis. Au banc, « une reception etait-elle armee ? »
 * sera la premiere question, et il faut pouvoir y repondre a l'instant ou on se
 * la pose.
 */
typedef struct {
    bool    rx_armee;       /* une reception DMA est en file */
    bool    maitre_vu;      /* le maitre a touche le tampon au moins une fois */
    uint32_t touches;       /* combien de fois — grimpe si le fil est vivant */
    uint32_t annulations;   /* annulations relayees (console seulement) */
    bool    sonnette_vue;   /* une reference de sonnette a ete prise */
    uint8_t sonnette;       /* derniere valeur de sonnette servie */
    uint8_t segment_type;   /* dernier segment publie : type... */
    uint8_t segment_num;    /* ...numero... */
    uint16_t segment_len;   /* ...et longueur */
} link_spi_diag_t;

/* Renseigne `out`. Rend false si le lien n'est pas installe. */
bool link_spi_diag(link_spi_diag_t *out);
