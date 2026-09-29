#include "sec_time.h"

#include <stddef.h>

/*
 * Etat global, comme sec_confirm : il n'y a qu'un coffre et qu'une heure. Les
 * tests appellent sec_time_reset() en entree de chaque cas, ce qui les garde
 * independants les uns des autres.
 */
static bool     s_valid;
static uint64_t s_base_unix;
static uint32_t s_base_ms;

void sec_time_reset(void)
{
    s_valid = false;
    s_base_unix = 0;
    s_base_ms = 0;
}

bool sec_time_set(uint64_t unix_s, uint32_t now_ms)
{
    /* Le plancher attrape une heure qui n'en est pas une — zero, variable non
     * initialisee, epoque de compilation. On ne retient RIEN dans ce cas :
     * retenir une heure douteuse en la marquant valide serait exactement le
     * mensonge que « NO TIME » existe pour eviter. */
    if (unix_s < SEC_TIME_MIN_PLAUSIBLE) {
        return false;
    }
    s_base_unix = unix_s;
    s_base_ms = now_ms;
    s_valid = true;
    return true;
}

bool sec_time_is_valid(void)
{
    return s_valid;
}

bool sec_time_now(uint32_t now_ms, uint64_t *out_unix)
{
    if (!s_valid || out_unix == NULL) {
        return false;
    }
    /* Soustraction NON SIGNEE sur 32 bits : le rebouclage du compteur de
     * millisecondes se traverse correctement tant que l'ecart reste sous
     * ~49,7 jours, ce qui est le cas par construction (le coffre n'existe que
     * branche). Une soustraction signee, ou une comparaison « si now < base »,
     * rendrait ici une heure de quarante-neuf jours dans le passe. */
    const uint32_t ecoule_ms = now_ms - s_base_ms;
    *out_unix = s_base_unix + (uint64_t)(ecoule_ms / 1000u);
    return true;
}

uint64_t sec_time_totp_counter(uint64_t unix_s)
{
    return unix_s / SEC_TIME_TOTP_STEP;
}

uint8_t sec_time_window_remaining(uint64_t unix_s)
{
    /* Dans 1..30, jamais 0 : voir l'en-tete. */
    return (uint8_t)(SEC_TIME_TOTP_STEP - (unix_s % SEC_TIME_TOTP_STEP));
}
