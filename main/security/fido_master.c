/* main/security/fido_master.c — la cle maitresse FIDO.
 *
 * DECISION, PAS UN OUBLI : AUCUN eFUSE N'EST GRILLE ICI.
 *
 * La spec (docs/superpowers/specs/2026-08-17-fido2-webauthn-design.md) prevoit
 * K_maitre dans un bloc eFuse en lecture protegee, derriere le peripherique
 * HMAC materiel du P4. Griller un eFuse est IRREVERSIBLE, et la proprietaire
 * ne l'a pas autorise — voir les trois ecarts assumes en tete de
 * .superpowers/sdd/2026-08-17-fido2-plan-1/contraintes-globales.md.
 *
 * Ce module met donc K_maitre en NVS en clair, sur LES TROIS cartes (pas
 * seulement le devkit), avec un HMAC-SHA256 logiciel (mbedtls) plutot que le
 * peripherique HMAC materiel. C'est strictement moins sur que la spec : un
 * dump de la partition NVS revele la cle maitresse, ce qu'un eFuse en lecture
 * protegee interdirait. Le passage a l'eFuse est un TRAVAIL SEPARE, a
 * demander explicitement — il ne se fait pas silencieusement dans une tache
 * de derivation.
 *
 * Consequence acceptee : un identifiant cree sur une carte ne fonctionnera
 * jamais sur une autre tant que cette cle n'est pas partagee — ce qui est de
 * toute facon le comportement voulu (deux cartes, deux maitres, deux univers
 * d'identifiants).
 */
#include "fido_master.h"
#include "sec_config.h"     /* STORAGE_NAMESPACE — divergence assumee vs KeSp */
#include "nvs_utils.h"
#include "esp_random.h"
#include "esp_log.h"
#include "mbedtls/md.h"
#include <string.h>

#define FIDO_MASTER_KEY_LEN 32u

static const char *TAG = "fido_master";

static uint8_t s_master_key[FIDO_MASTER_KEY_LEN];
static bool    s_master_key_loaded = false;

/* Charge K_maitre depuis NVS ; la genere par esp_fill_random() et la
 * persiste au tout premier appel (partition vierge ou cle absente). Chargee
 * une seule fois par cycle d'alimentation : rien ne change K_maitre en vie,
 * donc rien ne justifie de relire NVS a chaque HMAC. */
static void fido_master_ensure_loaded(void)
{
    if (s_master_key_loaded) return;

    uint32_t unused_total = 0;
    esp_err_t err = nvs_load_blob_with_total(STORAGE_NAMESPACE, "fido_master",
                                              s_master_key, sizeof(s_master_key),
                                              "fido_master_ver", &unused_total);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "aucune cle maitresse FIDO en NVS (%s) — generation",
                 esp_err_to_name(err));
        esp_fill_random(s_master_key, sizeof(s_master_key));
        esp_err_t save_err = nvs_save_blob_with_total(STORAGE_NAMESPACE, "fido_master",
                                                        s_master_key, sizeof(s_master_key),
                                                        "fido_master_ver", 1);
        if (save_err != ESP_OK) {
            /* La cle generee reste utilisable pour cette session (elle est
             * en RAM), mais un reset la perdrait : tous les credentials
             * emis avant que NVS ne fonctionne redeviendraient invalides.
             * Signale, pas fatal — pas de bouton reset sur le coffre, donc
             * pas de recuperation manuelle possible de toute facon. */
            ESP_LOGE(TAG, "echec de persistance de la cle maitresse FIDO: %s",
                     esp_err_to_name(save_err));
        }
    }
    s_master_key_loaded = true;
}

void fido_master_hmac(const uint8_t *msg, size_t len, uint8_t out[32])
{
    fido_master_ensure_loaded();

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_setup(&ctx, info, 1 /* mode HMAC */);
    mbedtls_md_hmac_starts(&ctx, s_master_key, sizeof(s_master_key));
    mbedtls_md_hmac_update(&ctx, msg, len);
    mbedtls_md_hmac_finish(&ctx, out);
    mbedtls_md_free(&ctx);
}
