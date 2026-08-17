#include "sec_gate.h"

#include "esp_log.h"

#include "board.h"
#include "sec_confirm.h"

static const char *TAG = "sec_gate";

#if BOARD_CONFIRM_SOURCE == BOARD_CONFIRM_LINK

esp_err_t sec_gate_init(void)
{
    /* La source réelle est le lien SPI. Tant qu'il n'est pas écrit, aucune
     * confirmation n'est accordée et les opérations expirent. */
    ESP_LOGW(TAG, "lien S3 pas encore implémenté — toute confirmation expirera");
    return ESP_OK;
}

const char *sec_gate_source(void)
{
    return "lien S3 (non implémenté)";
}

#elif BOARD_CONFIRM_SOURCE == BOARD_CONFIRM_BUTTON

esp_err_t sec_gate_init(void)
{
    /* Le GPIO est configure par hmi_init(), pas ici : ce module ne connait que
     * la source, pas le materiel. */
    ESP_LOGI(TAG, "confirmation par bouton en facade");
    return ESP_OK;
}

const char *sec_gate_source(void)
{
    return "bouton en facade";
}

void sec_gate_button_confirm(uint32_t pressed_at_ms)
{
    /* Ne fait que relayer : c'est sec_confirm qui decide, et il refuse hors
     * d'une operation armee. Meme contrat que la variante console. */
    sec_confirm_authorize(pressed_at_ms);
}

#else

esp_err_t sec_gate_init(void)
{
    ESP_LOGW(TAG, "carte sans source physique de confirmation (%s) : confirmation par la console « sec confirm »",
             BOARD_NAME);
    ESP_LOGW(TAG, "béquille de développement — aucune valeur de sécurité");
    return ESP_OK;
}

const char *sec_gate_source(void)
{
    return "console (béquille de développement)";
}

#endif /* BOARD_CONFIRM_SOURCE */

#if BOARD_CONSOLE_ACTIONS
/*
 * Appelée par la commande console. Ne fait que relayer : c'est sec_confirm qui
 * décide, et il refuse hors d'une opération armée.
 *
 * Conditionné à BOARD_CONSOLE_ACTIONS SEUL, pas à la branche BOARD_CONFIRM_SOURCE
 * ci-dessus : les deux axes divergent sur wt9932_key (BOARD_CONFIRM_BUTTON avec
 * la console quand même laissée au dev), donc cette béquille doit rester
 * accessible qu'on soit dans la branche "aucune source" (jc_devkit) ou "bouton"
 * (wt9932_key). Seule niphar_chest (BOARD_CONSOLE_ACTIONS=0) ne la compile pas.
 */
void sec_gate_console_confirm(uint32_t pressed_at_ms)
{
    sec_confirm_authorize(pressed_at_ms);
}
#endif
