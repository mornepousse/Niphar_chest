#include "sec_confirm.h"

/* CONCURRENCY MODEL (Phase-2 pentest, 2026-06-11) — why no lock is needed.
 * sec_confirm is a pure module (host-tested, no FreeRTOS). The three entry
 * points run from at most TWO contexts, and the security personalities are
 * mutually exclusive at build (OpenPGP CCID xor OTP-HID), so per build:
 *   - arm()  + poll()  run on the SAME task (ccid_worker in OpenPGP, tud_task
 *     in OTP) — serialized with each other, so the (state,slot) pair is always
 *     written and read consistently.
 *   - authorize() runs on keyboard_task (the K_SEC_CONFIRM keypress), which is
 *     the ONLY cross-task writer. It performs a single PENDING->AUTHORIZED flip
 *     and never touches s_slot.
 * Each field is a byte / aligned word: a naturally-aligned single-word load
 * or store is atomic on the P4's RISC-V HP core (ESP32-P4 datasheet, p.37,
 * §4.1.1.1 "High-Performance CPU" — dual-core 32-bit RISC-V; this used to say
 * "LX7", inherited from KeSp_firmware which targets the Xtensa-based S3 —
 * wrong core for this project). The only race is authorize() vs poll()'s
 * timeout branch at the 15 s boundary; its worst case is a real touch
 * accepted at T+15.0s instead of rejected (or vice-versa) — benign, since the
 * user physically touched for the armed slot (and the caller re-checks the
 * slot). No touch can be fabricated and no wrong slot can be granted. If a
 * SECOND poll context is ever added (two enabled personalities, or a
 * concurrent admin path), wrap the read-modify-write sections in a portMUX
 * critical section (gated for the host build).
 *   - peek() is READ-ONLY and adds no writer, but it reads TWO fields
 *     (s_state and s_armed_ms) from a third context (hmi_task on the key
 *     board), while arm() writes them as separate, non-atomic stores
 *     (s_state, then s_slot, then s_armed_ms). A peek() interleaved between
 *     those stores can observe a torn snapshot: the new s_state == PENDING
 *     paired with the PREVIOUS s_armed_ms. If that stale timestamp is already
 *     >= SEC_CONFIRM_TIMEOUT_MS in the past, peek() reports TIMEDOUT for a
 *     request that was in fact just armed — a transient, self-correcting
 *     display glitch (the next peek(), after arm()'s stores complete, reads
 *     the fresh s_armed_ms and reports PENDING again). This CANNOT fabricate
 *     AUTHORIZED: that return value depends only on s_state, never on
 *     s_armed_ms, and s_state itself is a single aligned store/load, so
 *     peek() never observes a torn s_state. No grant can be shown, and none
 *     can be consumed, that authorize() did not actually produce. Scope of
 *     this guarantee: it covers peek() racing arm() only; it does NOT claim
 *     the analysis above is otherwise unchanged. Do NOT let a display path
 *     call poll(). */

static sec_confirm_state_t s_state    = SEC_CONFIRM_IDLE;
static uint8_t             s_slot     = 0;
static uint32_t            s_armed_ms = 0;

void sec_confirm_reset(void)
{
    s_state    = SEC_CONFIRM_IDLE;
    s_slot     = 0;
    s_armed_ms = 0;
}

void sec_confirm_arm(uint8_t slot, uint32_t now_ms)
{
    s_state    = SEC_CONFIRM_PENDING;
    s_slot     = slot;
    s_armed_ms = now_ms;
}

void sec_confirm_authorize(void)
{
    if (s_state == SEC_CONFIRM_PENDING)
        s_state = SEC_CONFIRM_AUTHORIZED;
}

sec_confirm_state_t sec_confirm_poll(uint32_t now_ms, uint8_t *out_slot)
{
    if (s_state == SEC_CONFIRM_PENDING &&
        (now_ms - s_armed_ms) >= SEC_CONFIRM_TIMEOUT_MS) {
        s_state = SEC_CONFIRM_IDLE;
        return SEC_CONFIRM_TIMEDOUT;
    }
    if (s_state == SEC_CONFIRM_AUTHORIZED) {
        if (out_slot) *out_slot = s_slot;
        s_state = SEC_CONFIRM_IDLE;
        return SEC_CONFIRM_AUTHORIZED;
    }
    return s_state;
}

sec_confirm_state_t sec_confirm_peek(uint32_t now_ms)
{
    /* Meme expression d'echeance que poll(), volontairement dupliquee plutot
     * que factorisee derriere un predicat partage. Un predicat pur commun
     * serait effectivement plus sur en theorie (meme auditabilite, zero
     * divergence possible) — mais poll() est sur le chemin de signature et
     * deja audite ; le remanier pour un gain purement preventif, sur une
     * expression de deux lignes et stable, est un risque non nul contre un
     * benefice speculatif. A rouvrir si ce calcul d'echeance se complexifie
     * un jour. */
    if (s_state == SEC_CONFIRM_PENDING &&
        (now_ms - s_armed_ms) >= SEC_CONFIRM_TIMEOUT_MS) {
        return SEC_CONFIRM_TIMEDOUT;
    }
    return s_state;
}
