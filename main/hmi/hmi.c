#include "hmi/hmi.h"

#include "board.h"

#if BOARD_CONFIRM_SOURCE == BOARD_CONFIRM_BUTTON

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"

#include "hmi/button_debounce.h"
#include "hmi/led_state.h"
#include "sec_confirm.h"
#include "sec_gate.h"
#include "usb/usb_mode.h"

static const char *TAG = "hmi";

/*
 * Instantane publie pour hmi/screen.c, sous verrou.
 *
 * Modele identique a storage/sd_card.c:s_lock — allocation statique (une
 * creation qui echoue laisserait un verrou nul, exactement le bug qu'on
 * fermerait sinon), verrou pris pour la duree de la copie seulement, jamais
 * pendant un transfert materiel (ici, aucun ; l'I2C vit dans screen.c, dans
 * une autre tache).
 */
static StaticSemaphore_t s_snap_lock_buf;
static SemaphoreHandle_t s_snap_lock;
static hmi_snapshot_t    s_snapshot;

void hmi_snapshot(hmi_snapshot_t *out)
{
    if (s_snap_lock == NULL) {
        /* hmi_init() n'a jamais tourne (ou son propre echec l'a laisse a
         * NULL) : rendre l'instantane zero-initialise plutot que de lire une
         * structure jamais ecrite. */
        *out = (hmi_snapshot_t){ 0 };
        return;
    }
    xSemaphoreTake(s_snap_lock, portMAX_DELAY);
    *out = s_snapshot;
    xSemaphoreGive(s_snap_lock);
}

/* 5 ms : quatre echantillons par periode d'anti-rebond, assez pour que le
 * filtre voie une ligne qui se calme, et assez rare pour ne rien couter. */
#define HMI_TICK_MS         5
#define HMI_FLASH_MS      120
/* La periode de l'alternance (LED_ALTERNATE_MS) vit dans hmi/led_state.h,
 * avec la fonction pure led_wait_phase() qui en decoule : c'est elle qui
 * decide la phase, ce fichier ne fait plus que lui fournir l'instant
 * d'armement. */

static led_strip_handle_t s_strip;
static btn_debounce_t     s_mode_btn;
static btn_debounce_t     s_confirm_btn;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Actif bas : la broche est tiree a la masse quand le doigt appuie. */
static bool read_btn(gpio_num_t pin)
{
    const int level = gpio_get_level(pin);
    return BOARD_BTN_ACTIVE_LOW ? (level == 0) : (level != 0);
}

static void paint(led_view_t v, uint32_t t, uint32_t wait_armed_at)
{
    uint8_t r = v.rgb.r, g = v.rgb.g, b = v.rgb.b;

    if (v.pattern == LED_PATTERN_OFF) {
        r = g = b = 0;
    } else if (v.pattern == LED_PATTERN_ALTERNATE) {
        /* Bascule franche, pas un fondu. La phase vient de led_wait_phase()
         * (hmi/led_state.h), relative a wait_armed_at et non a l'horloge
         * murale : c'est ce qui garantit que la toute premiere phase vue par
         * l'utilisateur est celle du mode, quel que soit l'instant
         * d'armement. Aucune couleur en dur ici — les deux viennent de
         * led_state_view(), cette fonction ne fait que choisir laquelle
         * regarder. */
        const led_rgb_t c = (led_wait_phase(wait_armed_at, t) == LED_WAIT_PHASE_PRIMARY)
                                 ? v.rgb : v.rgb_alt;
        r = c.r; g = c.g; b = c.b;
    }

    /* Une LED muette ne doit jamais bloquer une operation : on journalise au
     * pire, on ne remonte pas l'erreur. */
    if (led_strip_set_pixel(s_strip, 0, r, g, b) == ESP_OK) {
        (void)led_strip_refresh(s_strip);
    }
}

static void hmi_task(void *arg)
{
    (void)arg;
    led_event_t         event           = LED_EVENT_NONE;
    uint32_t            event_until     = 0;
    uint32_t            event_started_at = 0;
    sec_confirm_state_t last_state      = SEC_CONFIRM_IDLE;
    bool                was_pending     = false;
    uint32_t            wait_armed_at   = 0;

    for (;;) {
        const uint32_t t = now_ms();

        if (btn_feed(&s_mode_btn, read_btn(BOARD_BTN_MODE), t) == BTN_PRESSED) {
            const esp_err_t err = usb_mode_cycle_next();
            /* Le flash annonce le NOUVEAU mode, donc il se lit apres la
             * bascule ; en cas d'echec, c'est un refus qu'on montre. */
            event            = (err == ESP_OK) ? LED_EVENT_MODE : LED_EVENT_REFUSED;
            event_until      = t + HMI_FLASH_MS;
            event_started_at = t;
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "bascule de mode : %s", esp_err_to_name(err));
            }
        }

        if (btn_feed(&s_confirm_btn, read_btn(BOARD_BTN_CONFIRM), t) == BTN_PRESSED) {
            /* peek AVANT d'autoriser : c'est la seule facon de savoir si
             * l'appui a servi a quelque chose. Ne rien flasher hors d'une
             * operation armee, pour ne pas suggerer qu'il s'est passe quelque
             * chose. */
            if (sec_confirm_peek(t) == SEC_CONFIRM_PENDING) {
                sec_gate_button_confirm(t);
                event            = LED_EVENT_GRANTED;
                event_until      = t + HMI_FLASH_MS;
                event_started_at = t;
            }
        }

        if (event != LED_EVENT_NONE && (int32_t)(t - event_until) >= 0) {
            event = LED_EVENT_NONE;
        }

        /* peek_labeled() et jamais poll(), et jamais peek() ici : consommer
         * volerait la permission a la tache qui attend de signer, et deux
         * appels separes (etat puis operation) rouvriraient la fenetre de
         * course qu'un seul appel ferme — voir le modele de concurrence en
         * tete de security/sec_confirm.c, bullet peek_labeled(). `op` sert a
         * la fois a rien ici (la LED ne porte pas de libelle) et a
         * l'instantane publie plus bas pour hmi/screen.c — un seul appel pour
         * les deux usages, jamais deux lectures. */
        sec_op_t op = SEC_OP_UNKNOWN;
        const sec_confirm_state_t st = sec_confirm_peek_labeled(t, &op);
        /* Appelee IMMEDIATEMENT apres peek_labeled(), rien entre les deux :
         * c'est ce qui garde la fenetre de course sur l'etiquette a la meme
         * echelle (quelques cycles CPU, pas un appel arbitrairement plus
         * tard) que celle deja toleree par peek_labeled() elle-meme entre
         * s_op et s_state — voir sec_confirm_label() dans sec_confirm.h. */
        char label[OATH_NAME_DISPLAY_MAX];
        memcpy(label, sec_confirm_label(), sizeof(label));
        const bool pending = (st == SEC_CONFIRM_PENDING);

        /* Sur la TRANSITION vers l'attente : c'est l'instant que
         * led_wait_phase() doit voir comme origine de la phase. Le memoriser
         * SEULEMENT au moment ou l'attente s'arme — pas a chaque tick — pour
         * que la phase ne re-parte jamais de zero pendant qu'on attend
         * encore. */
        if (pending && !was_pending) {
            wait_armed_at = t;
        }
        was_pending = pending;

        /* Sur la TRANSITION vers l'expiration, jamais sur l'etat. peek() ne
         * consomme rien — c'est tout son interet — donc TIMEDOUT reste vrai
         * tant que personne n'appelle poll(). Declencher sur l'etat ferait
         * clignoter le rouge indefiniment des que le consommateur tarde. */
        if (st == SEC_CONFIRM_TIMEDOUT && last_state != SEC_CONFIRM_TIMEDOUT) {
            event            = LED_EVENT_REFUSED;
            event_until      = t + HMI_FLASH_MS;
            event_started_at = t;
        }
        last_state = st;

        paint(led_state_view(usb_mode_get(), pending, event), t, wait_armed_at);

        /* Publication sous verrou, en tout dernier — apres que ce tick a
         * fini de decider event/pending/wait_armed_at, jamais avant : un
         * lecteur de screen.c ne doit jamais voir un instantane a moitie a
         * jour pour ce tick. */
        if (s_snap_lock != NULL) {
            /* Pas const : un tableau ne se copie pas par une expression
             * `.label = label` dans un initialisateur designe — memcpy()
             * juste apres, avant que quoi que ce soit d'autre ne touche
             * `snap`. */
            hmi_snapshot_t snap = {
                .mode            = usb_mode_get(),
                .confirm_pending = pending,
                .armed_at_ms     = wait_armed_at,
                .op              = op,
                .event           = event,
                .event_at_ms     = event_started_at,
            };
            memcpy(snap.label, label, sizeof(snap.label));
            xSemaphoreTake(s_snap_lock, portMAX_DELAY);
            s_snapshot = snap;
            xSemaphoreGive(s_snap_lock);
        }

        vTaskDelay(pdMS_TO_TICKS(HMI_TICK_MS));
    }
}

esp_err_t hmi_init(void)
{
    /* Avant tout le reste : screen.c doit pouvoir lire un instantane (fut-il
     * zero-initialise) meme si la suite de cette fonction echoue plus loin. */
    if (s_snap_lock == NULL) {
        s_snap_lock = xSemaphoreCreateMutexStatic(&s_snap_lock_buf);
    }

    const gpio_config_t btns = {
        .pin_bit_mask = (1ULL << BOARD_BTN_MODE) | (1ULL << BOARD_BTN_CONFIRM),
        .mode         = GPIO_MODE_INPUT,
        /* Pull-up interne : les boutons sont cables vers la masse, sans aucun
         * composant externe. */
        .pull_up_en   = BOARD_BTN_ACTIVE_LOW ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = BOARD_BTN_ACTIVE_LOW ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&btns);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "configuration des boutons : %s", esp_err_to_name(err));
        return err;
    }

    const led_strip_config_t strip_cfg = {
        .strip_gpio_num = BOARD_LED_WS2812,
        .max_leds       = BOARD_LED_COUNT,
    };
    const led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,   /* 10 MHz : 0,1 us par tick */
    };
    err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED indisponible : %s — la cle reste utilisable", esp_err_to_name(err));
        return err;
    }
    (void)led_strip_clear(s_strip);

    btn_init(&s_mode_btn);
    btn_init(&s_confirm_btn);

    if (xTaskCreate(hmi_task, "hmi", 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "creation de la tache IHM impossible");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "boutons IO%d (mode) / IO%d (confirmation), LED IO%d",
             BOARD_BTN_MODE, BOARD_BTN_CONFIRM, BOARD_LED_WS2812);
    return ESP_OK;
}

#else /* pas de bouton sur cette carte */

esp_err_t hmi_init(void)
{
    return ESP_OK;   /* no-op : main.c n'a pas a savoir quelle carte tourne */
}

void hmi_snapshot(hmi_snapshot_t *out)
{
    /* Pas de hmi_task ici pour publier quoi que ce soit : le zero-init est le
     * seul instantane correct — mode NONE, rien en attente, aucun evenement.
     * Une carte sans bouton n'a d'ailleurs jamais BOARD_OLED_SCL non plus, donc
     * personne n'appelle cette fonction en pratique ; elle existe pour que le
     * lien reste correct si un jour l'un des deux existe sans l'autre. */
    *out = (hmi_snapshot_t){ 0 };
}

#endif
