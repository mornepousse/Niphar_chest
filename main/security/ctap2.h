/* main/security/ctap2.h — reponse CTAP2 authenticatorGetInfo, en logique
 * pure : construit des octets a partir de constantes fixes, aucun appel
 * ESP-IDF, aucun octet lu depuis l'hote (contrairement a ctaphid.c/apdu.c,
 * ce module n'a rien a valider en entree).
 *
 * Tache 5 du plan FIDO2 : seul authenticatorGetInfo (sous-commande CBOR
 * 0x04) est couvert. Le decodeur CBOR general est reporte a la tache qui en
 * aura un vrai besoin (makeCredential, plan 2) — voir contraintes-globales.md,
 * ecart assume 2 : ecrire un decodeur sans consommateur, c'est ecrire du
 * code jamais eprouve.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Sous-commande CTAP2 portee par le premier octet d'une charge utile
 * CTAPHID_CBOR. Seule authenticatorGetInfo est reconnue a ce stade. */
#define CTAP2_CMD_GET_INFO 0x04u

/*
 * Codes de statut CTAP2 — DISTINCTS des codes d'erreur CTAPHID_ERR_* de
 * ctaphid.h. Ceux de ctaphid.h concernent le TRANSPORT (canal, sequence de
 * paquets) et voyagent dans une trame CTAPHID_CMD_ERROR (0x3F). Ceux-ci
 * concernent la commande CTAP2 elle-meme et voyagent comme PREMIER OCTET
 * d'une reponse portee par une trame CTAPHID_CMD_CBOR — jamais une trame
 * ERROR. Melanger les deux tromperait un client CTAP2 correctement ecrit.
 */
#define CTAP2_OK                  0x00u /* succes : le CBOR qui suit est la reponse */
#define CTAP2_ERR_INVALID_COMMAND 0x01u /* sous-commande CBOR non reconnue */
#define CTAP2_ERR_INVALID_LENGTH  0x03u /* charge CTAPHID_CBOR vide (pas d'octet de commande) */
#define CTAP2_ERR_OTHER           0x7Fu /* erreur generique — ex. echec interne de construction
                                          * de la reponse, qui n'a rien a voir avec la longueur
                                          * de la REQUETE (voir mode_fido.c) */

/*
 * Construit la reponse complete a authenticatorGetInfo dans `out` (capacite
 * `cap` octets) : le statut CTAP2_OK suivi de la map CBOR canonique —
 * versions (1), aaguid (3), options (4), maxMsgSize (5), dans cet ordre
 * (voir ctap2.c pour pourquoi cet ordre precis et pas un autre). Rend le
 * nombre total d'octets ecrits (statut compris), ou 0 si `cap` est
 * insuffisant — jamais de troncature silencieuse, meme discipline que
 * cbor_enc.h. `out` NULL ou `cap` nul rend aussi 0.
 */
size_t ctap2_build_get_info(uint8_t *out, size_t cap);
