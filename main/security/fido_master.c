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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

#define FIDO_MASTER_KEY_LEN 32u

/* Version de schema du blob NVS — comparee au chargement, pas seulement
 * ecrite. Un format de blob futur incompatible (taille ou structure changee)
 * doit regenerer K_maitre plutot que d'interpreter des octets d'un autre
 * schema comme une cle valide. */
#define FIDO_MASTER_SCHEMA_VERSION 1u

static const char *TAG = "fido_master";

static uint8_t s_master_key[FIDO_MASTER_KEY_LEN];
static bool    s_master_key_loaded = false;

/*
 * Verrou protegeant TOUT l'etat partage ci-dessus : le drapeau de
 * chargement, la cle elle-meme, et la sequence "tester puis generer puis
 * persister puis poser le drapeau" qui les accompagne. Le P4 est bicoeur et
 * rien ne garantit que la tache 7 n'appellera fido_master_hmac() que depuis
 * une seule tache FreeRTOS : sans ce verrou, deux premiers appels
 * concurrents verraient tous deux le drapeau a faux, genereraient chacun
 * une cle differente, et se les ecraseraient mutuellement pendant qu'un
 * HMAC serait deja en cours avec l'ancienne valeur — une corruption
 * silencieuse de la cle maitresse, pas seulement une cle qui change au
 * redemarrage.
 *
 * Alloue statiquement (StaticSemaphore_t), comme sd_card.c/usb_mode.c/
 * hmi.c : une creation qui echouerait laisserait un verrou nul, donc
 * exactement le bug qu'on ferme. Contrairement a ces trois fichiers, la
 * creation n'est pas faite dans un ..._init() explicite : ce module n'a pas
 * encore d'appelant (la tache 7 sera le premier), et imposer un ordre
 * d'appel ("initialise avant d'utiliser") serait exactement la promesse
 * fragile que ce correctif remplace. La creation est donc faite en retard
 * (paresseuse), mais la course sur la creation ELLE-MEME est fermee par un
 * spinlock portMUX — initialise statiquement, donc sans la meme course que
 * celle qu'il ferme.
 */
static portMUX_TYPE      s_lock_init_spinlock = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;

static SemaphoreHandle_t fido_master_lock(void)
{
    if (s_lock != NULL) return s_lock;   /* chemin rapide, sans le spinlock */
    portENTER_CRITICAL(&s_lock_init_spinlock);
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    portEXIT_CRITICAL(&s_lock_init_spinlock);
    return s_lock;
}

/* Charge K_maitre depuis NVS ; la genere par esp_fill_random() et la
 * persiste au tout premier appel (partition vierge, cle absente, ou version
 * de schema inattendue). Chargee une seule fois par cycle d'alimentation :
 * rien ne change K_maitre en vie, donc rien ne justifie de relire NVS a
 * chaque HMAC — et donc rien, apres ce premier appel, ne justifie plus de
 * tenir le verrou pour lire s_master_key (voir fido_master_hmac). */
static void fido_master_ensure_loaded(void)
{
    SemaphoreHandle_t lock = fido_master_lock();
    if (lock != NULL) xSemaphoreTake(lock, portMAX_DELAY);

    if (!s_master_key_loaded) {
        uint32_t stored_ver = 0;
        esp_err_t err = nvs_load_blob_with_total(STORAGE_NAMESPACE, "fido_master",
                                                  s_master_key, sizeof(s_master_key),
                                                  "fido_master_ver", &stored_ver);
        bool schema_mismatch = (err == ESP_OK) && (stored_ver != FIDO_MASTER_SCHEMA_VERSION);
        if (schema_mismatch) {
            ESP_LOGW(TAG, "cle maitresse FIDO : version de schema %u inattendue (attendu %u) — regeneration",
                     (unsigned)stored_ver, (unsigned)FIDO_MASTER_SCHEMA_VERSION);
        }
        if (err != ESP_OK || schema_mismatch) {
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "aucune cle maitresse FIDO en NVS (%s) — generation",
                         esp_err_to_name(err));
            }
            esp_fill_random(s_master_key, sizeof(s_master_key));
            esp_err_t save_err = nvs_save_blob_with_total(STORAGE_NAMESPACE, "fido_master",
                                                            s_master_key, sizeof(s_master_key),
                                                            "fido_master_ver",
                                                            FIDO_MASTER_SCHEMA_VERSION);
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

    if (lock != NULL) xSemaphoreGive(lock);
}

void fido_master_hmac(const uint8_t *msg, size_t len, uint8_t out[32])
{
    fido_master_ensure_loaded();

    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) {
        /* Jamais vu en pratique (SHA-256 est toujours compile dans mbedtls
         * ici), mais un HMAC silencieusement faux serait pire qu'un HMAC a
         * zero visible dans les logs : jamais de sortie non initialisee. */
        ESP_LOGE(TAG, "SHA-256 indisponible dans mbedtls");
        memset(out, 0, 32);
        return;
    }

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, info, 1 /* mode HMAC */) != 0) {
        ESP_LOGE(TAG, "mbedtls_md_setup a echoue");
        mbedtls_md_free(&ctx);
        memset(out, 0, 32);
        return;
    }
    mbedtls_md_hmac_starts(&ctx, s_master_key, sizeof(s_master_key));
    mbedtls_md_hmac_update(&ctx, msg, len);
    mbedtls_md_hmac_finish(&ctx, out);
    mbedtls_md_free(&ctx);
}
