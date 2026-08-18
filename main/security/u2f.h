/* main/security/u2f.h — U2F (CTAP1) sur CTAPHID_MSG : VERSION, REGISTER,
 * AUTHENTICATE.
 *
 * Tache 7 du plan FIDO2 : dernier maillon de transport avant que la carte
 * soit un second facteur utilisable (GitHub, Google, GitLab...) — voir
 * .superpowers/sdd/2026-08-17-fido2-plan-1/tache-7-brief.md.
 *
 * PARTIEL, pas pur : la logique de dispatch et de mise en forme des
 * reponses est simple, mais ce module appelle fido_master_hmac() (mbedtls +
 * NVS), openpgp_crypto_p256_sign/pubkey() (mbedtls + RNG materiel via
 * esp_random.h) et sec_confirm_arm/poll() (l'etat partage, pas la logique
 * pure de sec_confirm.c). Rien de tout cela ne compile sur l'hote — voir
 * contraintes-globales.md, tableau des fichiers : u2f.{c,h} est marque
 * "partiel", pas "oui", et n'entre donc pas dans test/. Le seul calcul
 * vraiment pur qu'il utilise (l'encodage DER d'une signature) est isole
 * dans ecdsa_der.{c,h}, lui teste sur l'hote.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "apdu.h"
#include "ecdsa_der.h"
#include "fido_attest.h"

/*
 * Capacite maximale d'une reponse U2F : dimensionnee par le plus gros cas,
 * U2F_REGISTER — 0x05 || pubkey(65) || len(1) || keyHandle(32) || cert ||
 * sig(DER) || SW1SW2(2). `sizeof(fido_attest_cert_der)` est un vrai entier
 * constant de compilation (taille d'un tableau fixe), utilisable ici comme
 * dans un dimensionnement de tableau chez l'appelant (mode_fido.c) sans
 * connaitre par avance la taille exacte du certificat genere.
 */
#define U2F_RESP_MAX \
    (1u + 65u + 1u + 32u + sizeof(fido_attest_cert_der) + ECDSA_DER_MAX_LEN + 2u)

/*
 * Reinitialise toute attente de confirmation U2F en cours. A appeler a
 * chaque entree dans le mode FIDO (mode_fido_fs_config()/hs_config()), sur
 * le meme modele que ctaphid_reset(&s_asm) juste a cote : une confirmation
 * armee pour une session USB precedente ne doit pas survivre a une
 * deconnexion/reconnexion, ni s'appliquer a la requete d'un hote different.
 */
void u2f_reset(void);

/*
 * Traite un message U2F deja parse en APDU ISO 7816-4 (`apdu_parse()`,
 * apdu.h) — c'est le contenu integral d'une trame CTAPHID_MSG une fois
 * reassemblee par ctaphid_feed(). Ecrit dans `out` (capacite `cap`) les
 * donnees de reponse suivies des deux octets de statut SW1SW2 (toujours au
 * moins ces deux octets si `cap >= 2`), et rend le nombre total d'octets
 * ecrits — 0 seulement si `cap < 2` ou si `apdu`/`out` est nul.
 *
 * `now_ms` est l'horloge de sec_confirm (memes unites que
 * sec_confirm_arm()/sec_confirm_poll() — millisecondes, base arbitraire
 * mais monotone).
 *
 * Ce que cette fonction NE PEUT PAS prouver sur ce materiel : le bouton de
 * confirmation en facade est electriquement ouvert (mesure, voir
 * docs/HARDWARE.md) — REGISTER et AUTHENTICATE (P1=0x03) construisent donc
 * toujours leur reponse jusqu'a 0x6985 (conditions non satisfaites), jamais
 * jusqu'a la signature. C'est le comportement correct attendu par un
 * navigateur qui reessaie ; ce n'est PAS un signe que cette fonction est
 * incomplete.
 */
size_t u2f_handle_apdu(const apdu_t *apdu, uint8_t *out, size_t cap, uint32_t now_ms);

/*
 * Autotest de démarrage — exerce le chemin de signature COMPLET une fois,
 * avec des vecteurs fixes (jamais des données réelles d'hôte) : dérivation
 * `fido_key_derive`, `openpgp_crypto_p256_pubkey`, SHA-256, signature ET
 * vérification (`mbedtls_ecdsa_verify`, comme `openpgp_crypto_selftest()`),
 * encodage DER (`ecdsa_der_encode`) — puis la même séquence signature+
 * vérification avec la clé d'ATTESTATION (`fido_attest_key`), vérifiée
 * contre `fido_attest_pubkey` (extraite du certificat, voir fido_attest.h) :
 * un test qui prouve au passage que la clé et le certificat committés
 * forment bien une paire.
 *
 * Raison d'être : ni REGISTER ni AUTHENTICATE ne peuvent jamais dépasser
 * `0x6985` sur ce matériel (bouton de confirmation électriquement ouvert,
 * voir u2f_handle_apdu()) — donc SANS cet autotest, le chemin de signature
 * ne s'exécuterait JAMAIS avant le premier vrai enregistrement, avec une
 * pile (`usb_task`, voir usb_device.c) dimensionnée par analogie et jamais
 * éprouvée. Cet autotest fait tourner ce chemin, sur CETTE pile, au moment
 * où l'appelant (mode_fido.c) le déclenche — voir le commentaire de son
 * point d'appel pour pourquoi c'est garanti être `usb_task`.
 *
 * Rend faux au premier échec, avec un ESP_LOGE précis (TAG "u2f") sur
 * l'étape fautive — même discipline que openpgp_crypto_selftest(). N'a
 * aucun effet sur le fonctionnement de la clé si le résultat est faux (ce
 * n'est qu'un journal, pas une porte — au contraire de la crypto OpenPGP,
 * U2F n'a pas de SW dédié pour "crypto en panne" ; voir les préoccupations
 * du rapport de tâche 7 si ce point doit être reconsidéré).
 */
bool u2f_selftest(void);
