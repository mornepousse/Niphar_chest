#include "usb/usb_mode.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "sec_confirm.h"
#include "usb/mode_fido.h"
#include "usb/mode_oath.h"
#include "usb/mode_otp.h"
#include "usb/mode_pgp.h"
#include "usb/mode_storage.h"
#include "usb/msc_disk.h"
#include "usb/usb_device.h"
#include "usb/usb_mode_cycle.h"
#include "usb/usb_mode_state.h"

static const char *TAG = "usb_mode";

/* Valeur initiale d'une statique : USB_MODE_NONE, sans qu'il y ait besoin de
 * l'écrire — voir usb_mode.h sur pourquoi c'est délibéré. */
static usb_mode_t s_mode;

/*
 * s_mode décrit-il vraiment ce qui est installé ?
 *
 * Faux dès qu'une bascule échoue en cours de route : la pile a été démontée,
 * et ce qui devait la remplacer n'est pas en place. Sans ce drapeau, le
 * court-circuit « mode déjà courant » plus bas renverrait ESP_OK sans rien
 * installer, et le coffre resterait sans fonction USB en prétendant l'avoir.
 * Une bascule vers le même mode doit donc rester possible tant que l'état
 * n'est pas certain. Vrai après usb_mode_init() : rien n'est installé, et
 * c'est exactement ce que dit USB_MODE_NONE.
 */
static bool s_mode_known;

/*
 * Drapeau « occupé » — PAS un mutex qui couvrirait toute la bascule.
 *
 * Sur wt9932_key, deux tâches peuvent atteindre l'appelant unique de ce
 * fichier : hmi_task (bouton MODE) et la tâche REPL (commande console, sur
 * une carte où BOARD_CONSOLE_ACTIONS le permet). Avant cette branche il n'y
 * avait qu'un seul appelant ; c'est la somme des deux qui crée le défaut.
 *
 * usb_device.c documente que usbd n'est pas réentrant : tud_disconnect() et
 * tusb_deinit() touchent les mêmes registres DWC2 que tud_task_ext(), donc
 * deux bascules en vol en même temps corromprait cet état partagé. Il faut
 * donc un exclusion mutuelle — mais PAS une attente bloquante sur toute la
 * durée de la bascule (jusqu'à 15 s, le temps que le worker CCID sorte).
 * hmi_task est aussi ce qui fait vivre la LED ; la bloquer sur ce mutex
 * figerait l'affichage precisement pendant l'attente qu'elle doit signaler.
 * Le choix retenu est donc un refus immédiat : si une bascule est déjà en
 * cours, la nouvelle demande échoue tout de suite avec
 * ESP_ERR_INVALID_STATE plutôt que d'attendre son tour. Le mutex ci-dessous
 * ne protège QUE la lecture/écriture de s_busy, jamais l'opération entière.
 *
 * Statique, comme s_lock dans storage/sd_card.c : une création qui échoue
 * laisserait un verrou nul, donc exactement le bug qu'on ferme.
 */
static StaticSemaphore_t s_busy_lock_buf;
static SemaphoreHandle_t s_busy_lock;
static bool s_busy;

/* Rend true et pose s_busy si aucune bascule n'est en cours ; sinon rend
 * false sans rien changer. Section critique courte : elle ne fait que lire
 * et écrire le drapeau, jamais attendre après usb_device_uninstall() ou
 * l'installation d'un mode. */
static bool busy_try_acquire(void)
{
    /* usb_mode_init() peut rendre ESP_ERR_NO_MEM si xSemaphoreCreateMutexStatic()
     * echoue, et main.c journalise cet echec sans s'arreter (doctrine : un
     * echec d'init USB se journalise, il ne panique pas). Sans ce garde, le
     * premier appel ici ferait xSemaphoreTake(NULL, ...) — panique FreeRTOS.
     * Le chemin est quasi inatteignable (mutex statique sur buffer non nul),
     * mais le refus explicite coute une ligne. */
    if (s_busy_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_busy_lock, portMAX_DELAY);
    const bool was_busy = s_busy;
    if (!was_busy) {
        s_busy = true;
    }
    xSemaphoreGive(s_busy_lock);
    return !was_busy;
}

/* À appeler sur TOUS les chemins de sortie de usb_mode_set() une fois
 * l'acquisition réussie — y compris les retours d'erreur. Un oubli sur un
 * seul chemin laisserait s_busy posé pour toujours, et la carte-clé
 * refuserait indéfiniment toute bascule suivante. */
static void busy_release(void)
{
    xSemaphoreTake(s_busy_lock, portMAX_DELAY);
    s_busy = false;
    xSemaphoreGive(s_busy_lock);
}

esp_err_t usb_mode_init(void)
{
    /* Rien à installer : au démarrage rien n'est encore branché côté
     * matériel, donc il n'y a rien à désinstaller non plus. */
    s_mode = USB_MODE_NONE;
    s_mode_known = true;
    s_busy_lock = xSemaphoreCreateMutexStatic(&s_busy_lock_buf);
    s_busy = false;
    return s_busy_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

usb_mode_t usb_mode_get(void)
{
    return s_mode;
}

bool usb_mode_is_known(void)
{
    return s_mode_known;
}

esp_err_t usb_mode_set(usb_mode_t mode)
{
    /*
     * Refus immédiat, pas une file d'attente : voir le commentaire de
     * s_busy plus haut. Deux tâches peuvent appeler cette fonction sur
     * wt9932_key ; laisser la seconde attendre bloquerait celle des deux qui
     * fait vivre la LED pendant toute la durée d'une bascule.
     */
    if (!busy_try_acquire()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (mode == s_mode && s_mode_known) {
        busy_release();
        return ESP_OK;
    }

    if (mode >= USB_MODE_COUNT) {
        busy_release();
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "mode USB : %s -> %s", usb_mode_name(s_mode), usb_mode_name(mode));

    /*
     * I4 (revue finale de branche) : purge TOUTE confirmation armée avant que
     * le nouveau mode ne puisse jamais s'en servir, un seul endroit
     * symétrique pour les trois personnalités (CCID, OTP, FIDO) plutôt qu'un
     * appel dupliqué dans chaque *_stop(). ccid_worker() (security/ccid.c)
     * appelait déjà sec_confirm_reset() sur son propre chemin d'arrêt ;
     * mode_otp_stop() et mode_fido_stop() n'avaient pas d'équivalent, donc
     * l'écran pouvait afficher jusqu'à SEC_CONFIRM_TIMEOUT_MS (15 s)
     * l'opération d'un mode qui vient d'être désinstallé — un libellé faux,
     * pas une permission fausse (les slots sec_confirm sont disjoints par
     * personnalité et arm() écrase l'état précédent, donc aucun octroi
     * croisé n'en découle), mais exactement la classe d'erreur que
     * sec_confirm.c documente exister pour empêcher.
     *
     * PLACEMENT (revu à la revue finale de branche, I5, point 2) : cet appel
     * vivait juste ici, avant les *_stop() ci-dessous et avant
     * usb_device_uninstall() — ce qui purgeait la confirmation même quand la
     * désinstallation échouait ensuite et que l'ancien mode restait le mode
     * RÉELLEMENT actif (voir le bloc `if (err != ESP_OK)` plus bas, qui
     * restaure s_mode dans ce cas). Une confirmation légitime pour le mode
     * qui reste en place se retrouvait alors effacée par un événement qui ne
     * le concernait pas — l'utilisateur devait réappuyer pour rien. Déplacé
     * après le `if (err != ESP_OK)` : l'appel n'arrive plus que lorsque la
     * désinstallation a RÉELLEMENT réussi, donc seulement quand l'ancien
     * mode est en train de disparaître pour de bon — jamais sur un chemin
     * qui peut encore faire machine arrière. Toujours placé AVANT toute
     * installation du nouveau mode (mode_pgp_data_load()/mode_otp_start()/
     * mode_fido_start(), plus bas) : aucune fenêtre inverse où le nouveau
     * mode démarrerait avec une confirmation de l'ancien encore armée. Le
     * court-circuit « même mode » plus haut protège toujours ce cas-là
     * (rien n'est démonté, rien n'est purgé).
     */

    /*
     * Le worker CCID d'abord, la pile USB ensuite. Il survit à tous les
     * démontages (créé une fois, jamais détruit) et poste des callbacks sur la
     * file de tud_task pendant qu'il attend une confirmation physique —
     * jusqu'à 15 s. tud_deinit() détruit cette file : le laisser tourner
     * pendant la désinstallation, c'est écrire dans une file libérée. Voir la
     * divergence BLOQUANT 1 en tête de main/security/ccid.c.
     */
    if (s_mode == USB_MODE_PGP) {
        mode_pgp_stop();
    }

    /* Même raison côté HID : retirer la table OTP du répartiteur unique
     * (usb/hid_dispatch.h) avant que la pile ne se démonte, sur le modèle
     * exact du CCID ci-dessus. */
    if (s_mode == USB_MODE_OTP) {
        mode_otp_stop();
    }

    /* Même raison côté FIDO : retirer la table FIDO du répartiteur unique
     * avant que la pile ne se démonte, sur le modèle exact du OTP ci-dessus. */
    if (s_mode == USB_MODE_FIDO) {
        mode_fido_stop();
    }

    /* Le mode OATH partage le worker CCID avec le mode PGP : même contrainte
     * d'ordre, pour la même raison. Il débranche en plus son applet et efface
     * son contexte — qui peut porter un secret en clair mis en attente par un
     * PUT non confirmé. Voir mode_oath_stop(). */
    if (s_mode == USB_MODE_OATH) {
        mode_oath_stop();
    }

    /* À partir d'ici la pile est démontée ou en train de l'être : plus rien
     * n'est garanti tant qu'une installation n'a pas réussi. */
    s_mode_known = false;

    /*
     * Toujours désinstaller avant de réinstaller, même vers USB_MODE_NONE :
     * c'est le seul moyen honnête de faire voir à l'hôte une vraie
     * déconnexion avant la nouvelle énumération (ou l'absence de). C'est
     * aussi ce qui garantit que le stockage et le CCID ne coexistent
     * jamais : à tout instant, au plus un jeu de descripteurs est installé.
     */
    esp_err_t err = usb_device_uninstall();
    if (err != ESP_OK) {
        /*
         * Échec de la désinstallation : elle n'a alors RIEN détaché — ni
         * tud_disconnect(), ni tusb_deinit(), ni le PHY (contrat en tête de
         * usb_device.h). Le périphérique du mode PRÉCÉDENT est donc toujours
         * sur le bus, et c'est lui qu'il faut annoncer : poser USB_MODE_NONE
         * ici était la résurgence, par une autre porte, du s_mode qui ment.
         *
         * Conserver le mode n'est pas cosmétique — c'est ce qui fait repasser
         * la tentative suivante par le mode_pgp_stop() ci-dessus, donc referme
         * la porte des callbacks CCID AVANT le tusb_deinit() qui détruit la
         * file de tud_task (divergence BLOQUANT 1 en tête de
         * security/ccid.c). Avec NONE, cette étape était sautée et la sûreté
         * ne tenait plus que par la rémanence de s_shutdown.
         *
         * Et on NE réarme PAS le CCID au passage, malgré l'apparente
         * asymétrie avec mode_pgp_stop() : usb_device_uninstall() a déjà posé
         * son drapeau d'arrêt de tâche avant d'échouer, donc plus personne ne
         * fera tourner tud_task. Un CCID rouvert n'aurait rien pour répondre,
         * et son premier usbd_defer_func() irait dans une file que plus
         * personne ne vide — osal_queue_send() y attend sans limite de temps
         * (managed_components/espressif__tinyusb/src/osal/osal_freertos.h:282).
         * Le mode est fini ; la reprise passe par une nouvelle bascule, pas
         * par une réanimation.
         */
        ESP_LOGE(TAG, "désinstallation avant bascule : %s", esp_err_to_name(err));
        const usb_mode_state_t st = usb_mode_state_on_failure(s_mode, USB_MODE_FAIL_UNINSTALL);
        s_mode = st.mode;
        s_mode_known = st.known;
        busy_release();
        return err;
    }

    /* La désinstallation a RÉELLEMENT réussi : l'ancien mode est en train de
     * disparaître pour de bon, plus aucun chemin de retour arrière. C'est
     * seulement maintenant que purger la confirmation armée est sûr — voir
     * le commentaire I4/I5 plus haut. */
    sec_confirm_reset();

    if (mode == USB_MODE_NONE) {
        s_mode = USB_MODE_NONE;
        s_mode_known = true;
        busy_release();
        return ESP_OK;
    }

    int string_count = 0;
    const char **strings;
    const uint8_t *fs_cfg;
    const uint8_t *hs_cfg;

    if (mode == USB_MODE_STORAGE) {
        err = msc_disk_init();
        if (err != ESP_OK) {
            /* La désinstallation, elle, a réussi : plus aucune fonction n'est
             * exposée. Laisser s_mode sur l'ancienne valeur ferait mentir
             * usb_mode_get() ET court-circuiter la prochaine demande de ce
             * même mode, qui renverrait ESP_OK sans rien installer. */
            ESP_LOGE(TAG, "init du disque : %s", esp_err_to_name(err));
            const usb_mode_state_t st =
                usb_mode_state_on_failure(s_mode, USB_MODE_FAIL_AFTER_UNINSTALL);
            s_mode = st.mode;
            s_mode_known = st.known;
            busy_release();
            return err;
        }
        strings = mode_storage_strings(&string_count);
        fs_cfg = mode_storage_fs_config();
        hs_cfg = mode_storage_hs_config();
    } else if (mode == USB_MODE_PGP) {
        /* USB_MODE_PGP : la carte OpenPGP sur CCID (tâche 10). Rien à
         * initialiser ici — mode_pgp_fs_config()/hs_config() force le
         * démarrage du worker CCID au passage, voir mode_pgp.c. */
        strings = mode_pgp_strings(&string_count);
        fs_cfg = mode_pgp_fs_config();
        hs_cfg = mode_pgp_hs_config();
    } else if (mode == USB_MODE_OTP) {
        /* USB_MODE_OTP : la clé CR-HMAC sur HID (tâche 11). Rien à
         * initialiser ici non plus — mode_otp_fs_config()/hs_config() câble
         * les hooks otp_proto au passage, voir mode_otp.c. */
        strings = mode_otp_strings(&string_count);
        fs_cfg = mode_otp_fs_config();
        hs_cfg = mode_otp_hs_config();
    } else if (mode == USB_MODE_FIDO) {
        /* USB_MODE_FIDO : l'authentificateur U2F/CTAP-HID (plan FIDO2,
         * tâche 3). Rien à initialiser ici non plus — mode_fido_fs_config()/
         * hs_config() réarme l'assembleur CTAP-HID au passage, voir
         * mode_fido.c. */
        strings = mode_fido_strings(&string_count);
        fs_cfg = mode_fido_fs_config();
        hs_cfg = mode_fido_hs_config();
    } else {
        /* USB_MODE_OATH : les comptes TOTP sur CCID (plan OATH, tâche 6).
         * Comme pour PGP, rien à initialiser ici — mode_oath_fs_config()/
         * hs_config() force le démarrage du worker CCID au passage. Le magasin
         * et l'applet, eux, attendent que l'installation ait réussi : voir
         * mode_oath_start() plus bas. */
        strings = mode_oath_strings(&string_count);
        fs_cfg = mode_oath_fs_config();
        hs_cfg = mode_oath_hs_config();
    }

    err = usb_device_install(fs_cfg, hs_cfg, strings, string_count);
    if (err != ESP_OK) {
        /* Même raison qu'au-dessus : rien n'est installé, il faut le dire. */
        ESP_LOGE(TAG, "installation du mode %s : %s",
                 usb_mode_name(mode), esp_err_to_name(err));
        const usb_mode_state_t st =
            usb_mode_state_on_failure(s_mode, USB_MODE_FAIL_AFTER_UNINSTALL);
        s_mode = st.mode;
        s_mode_known = st.known;
        busy_release();
        return err;
    }

    if (mode == USB_MODE_PGP) {
        /*
         * Après coup, jamais avant : usb_device_install() vient de faire
         * tourner ccid_drv_init() (via tusb_init()), qui réarme les PIN
         * d'usine en RAM. Charger l'état persisté plus tôt se ferait
         * écraser. Voir le commentaire de mode_pgp_data_load() pour le choix
         * « à l'entrée du mode » plutôt qu'au démarrage (tâche 12).
         */
        mode_pgp_data_load();
    }

    if (mode == USB_MODE_OTP) {
        /* Après coup, jamais avant : tant que l'installation n'a pas
         * réussi, poser la table OTP au répartiteur HID (usb/
         * hid_dispatch.h) ferait répondre un mode qu'aucun hôte n'a encore
         * en face. Voir mode_otp_start() dans mode_otp.c. */
        mode_otp_start();
    }

    if (mode == USB_MODE_FIDO) {
        /* Après coup, jamais avant : sur le modèle exact du OTP ci-dessus.
         * Voir mode_fido_start() dans mode_fido.c. */
        mode_fido_start();
    }

    if (mode == USB_MODE_OATH) {
        /*
         * Après coup, jamais avant, et pour une raison plus dure que pour les
         * autres : usb_device_install() vient de faire tourner ccid_drv_init(),
         * qui remet l'aiguillage d'applet sur son défaut (OpenPGP). Poser
         * l'applet OATH plus tôt le ferait écraser, et l'hôte parlerait à la
         * carte OpenPGP en croyant parler à l'applet OATH. Voir
         * mode_oath_start() et ccid_set_applet() dans security/ccid.h.
         */
        mode_oath_start();
    }

    s_mode = mode;
    s_mode_known = true;
    busy_release();
    return ESP_OK;
}

esp_err_t usb_mode_cycle_next(void)
{
    return usb_mode_set(usb_mode_cycle_after(s_mode));
}
