#include "hmi/hmi.h"

#include "board.h"

#if BOARD_CONFIRM_SOURCE == BOARD_CONFIRM_BUTTON

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#include "hmi/button_debounce.h"
#include "hmi/led_state.h"
#include "sec_confirm.h"
#include "sec_gate.h"
#include "usb/usb_mode.h"

static const char *TAG = "hmi";

/* 5 ms : quatre echantillons par periode d'anti-rebond, assez pour que le
 * filtre voie une ligne qui se calme, et assez rare pour ne rien couter. */
#define HMI_TICK_MS      5
#define HMI_FLASH_MS   120
#define HMI_PULSE_MS  1000   /* 1 Hz */

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

static void paint(led_view_t v, uint32_t t)
{
    uint8_t r = v.rgb.r, g = v.rgb.g, b = v.rgb.b;

    if (v.pattern == LED_PATTERN_OFF) {
        r = g = b = 0;
    } else if (v.pattern == LED_PATTERN_PULSE) {
        /* Triangle symetrique : monte sur la premiere moitie de la periode,
         * descend sur la seconde. Pas de sinus — pas de flottant pour ca. */
        const uint32_t phase = t % HMI_PULSE_MS;
        const uint32_t half  = HMI_PULSE_MS / 2;
        const uint32_t k = (phase < half) ? phase : (HMI_PULSE_MS - phase);
        r = (uint8_t)((uint32_t)r * k / half);
        g = (uint8_t)((uint32_t)g * k / half);
        b = (uint8_t)((uint32_t)b * k / half);
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
    led_event_t         event        = LED_EVENT_NONE;
    uint32_t            event_until  = 0;
    sec_confirm_state_t last_state   = SEC_CONFIRM_IDLE;

    for (;;) {
        const uint32_t t = now_ms();

        if (btn_feed(&s_mode_btn, read_btn(BOARD_BTN_MODE), t) == BTN_PRESSED) {
            const esp_err_t err = usb_mode_cycle_next();
            /* Le flash annonce le NOUVEAU mode, donc il se lit apres la
             * bascule ; en cas d'echec, c'est un refus qu'on montre. */
            event       = (err == ESP_OK) ? LED_EVENT_MODE : LED_EVENT_REFUSED;
            event_until = t + HMI_FLASH_MS;
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
                sec_gate_button_confirm();
                event       = LED_EVENT_GRANTED;
                event_until = t + HMI_FLASH_MS;
            }
        }

        if (event != LED_EVENT_NONE && (int32_t)(t - event_until) >= 0) {
            event = LED_EVENT_NONE;
        }

        /* peek() et jamais poll() : consommer ici volerait la permission a la
         * tache qui attend de signer. Voir le modele de concurrence en tete de
         * security/sec_confirm.c. */
        const sec_confirm_state_t st = sec_confirm_peek(t);
        const bool pending = (st == SEC_CONFIRM_PENDING);

        /* Sur la TRANSITION vers l'expiration, jamais sur l'etat. peek() ne
         * consomme rien — c'est tout son interet — donc TIMEDOUT reste vrai
         * tant que personne n'appelle poll(). Declencher sur l'etat ferait
         * clignoter le rouge indefiniment des que le consommateur tarde. */
        if (st == SEC_CONFIRM_TIMEDOUT && last_state != SEC_CONFIRM_TIMEDOUT) {
            event       = LED_EVENT_REFUSED;
            event_until = t + HMI_FLASH_MS;
        }
        last_state = st;

        paint(led_state_view(usb_mode_get(), pending, event), t);
        vTaskDelay(pdMS_TO_TICKS(HMI_TICK_MS));
    }
}

esp_err_t hmi_init(void)
{
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

#endif
