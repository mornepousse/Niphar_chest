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

#else

esp_err_t sec_gate_init(void)
{
    ESP_LOGW(TAG, "carte sans lien : confirmation par la console « sec confirm »");
    ESP_LOGW(TAG, "béquille de développement — aucune valeur de sécurité");
    return ESP_OK;
}

const char *sec_gate_source(void)
{
    return "console (béquille de développement)";
}

#if BOARD_CONSOLE_ACTIONS
/* Appelée par la commande console. Ne fait que relayer : c'est sec_confirm qui
 * décide, et il refuse hors d'une opération armée. */
void sec_gate_console_confirm(void)
{
    sec_confirm_authorize();
}
#endif

#endif /* BOARD_CONFIRM_SOURCE == BOARD_CONFIRM_LINK */
