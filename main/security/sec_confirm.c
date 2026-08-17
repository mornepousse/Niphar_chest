#include "sec_confirm.h"

/* CONCURRENCY MODEL (Phase-2 pentest, 2026-06-11 ; relu et corrige a l'arrivee
 * de hmi_task, 2026-08-16) — why no lock is needed.
 * sec_confirm is a pure module (host-tested, no FreeRTOS). Entry points run
 * from at most THREE contexts, and the security personalities are mutually
 * exclusive at build (OpenPGP CCID xor OTP-HID), so per build:
 *   - arm() + poll() run on the SAME task (ccid_worker in OpenPGP — see
 *     dongle_confirm() in ccid.c, which also calls reset() — or usb_task
 *     running tud_task_ext() in OTP, see otp_hid.c) — serialized with each
 *     other, so the (state,slot) pair is always written and read
 *     consistently.
 *   - authorize() is called by whichever confirmation source main/security/
 *     sec_gate.c wires up for the board: the esp-console REPL task via the
 *     console-command crutch on any board with BOARD_CONSOLE_ACTIONS (see
 *     sec_gate.c, conditioned there and in console.c only — grep for it
 *     stays confined by scripts/fast.sh's guard-fou 4, so it isn't named
 *     here), and/or hmi_task via the button relay on a BOARD_CONFIRM_BUTTON
 *     board (main/hmi/hmi.c — the real physical gate). A prior revision of
 *     this comment named "keyboard_task (the K_SEC_CONFIRM keypress)" here:
 *     that task does not exist anywhere in this codebase (grep main/ turns
 *     up nothing) — inherited verbatim from KeSp_firmware, whose
 *     keyboard-side link this project doesn't have yet.
 *     wt9932_key runs BOTH crutches at once (BOARD_CONSOLE_ACTIONS=1 and
 *     BOARD_CONFIRM_BUTTON), so on that board the REPL task and hmi_task can
 *     call authorize() concurrently. Still safe: both perform the exact same
 *     idempotent PENDING->AUTHORIZED flip on a single aligned word and never
 *     touch s_slot, so a race between them produces the same outcome twice,
 *     never a torn or wrong-slot grant.
 * Each field is a byte / aligned word: a naturally-aligned single-word load
 * or store is atomic on the P4's RISC-V HP core (ESP32-P4 datasheet, p.37,
 * §4.1.1.1 "High-Performance CPU" — dual-core 32-bit RISC-V; this used to say
 * "LX7", inherited from KeSp_firmware which targets the Xtensa-based S3 —
 * wrong core for this project). Every task above (ccid_worker, usb_task, the
 * REPL task, hmi_task) is started with plain xTaskCreate() — none pinned via
 * xTaskCreatePinnedToCore — so IDF's SMP scheduler is free to run any of them
 * on either of the P4's two HP cores (never the LP core, which never runs
 * application FreeRTOS tasks). That doesn't weaken the atomicity claim: the
 * two HP cores are two instances of the same core the datasheet describes,
 * so a naturally-aligned single-word access is atomic the same way on
 * either one, and which HP core a task happens to run on changes nothing
 * about tearing. Cross-core placement only affects how fast one core's
 * write becomes visible to a reader on the other core — and that latency
 * window is exactly the transient staleness peek() already tolerates below.
 * The only race left is authorize() vs poll()'s timeout branch at the 15 s
 * boundary; its worst case is a real touch accepted at T+15.0s instead of
 * rejected (or vice-versa) — benign, since the user physically touched for
 * the armed slot (and the caller re-checks the slot). No touch can be
 * fabricated and no wrong slot can be granted. If a SECOND poll context is
 * ever added (two enabled personalities, or a concurrent admin path), wrap
 * the read-modify-write sections in a portMUX critical section (gated for
 * the host build).
 *   - peek() is READ-ONLY and adds no writer, but it reads TWO fields
 *     (s_state and s_armed_ms) from a third context — hmi_task, confirmed
 *     real as of main/hmi/hmi.c (this paragraph used to describe a task that
 *     didn't exist yet; it now matches the actual call site) — while arm()
 *     writes them as separate, non-atomic stores (s_state, then s_slot, then
 *     s_armed_ms). A peek() interleaved between those stores can observe a
 *     torn snapshot: the new s_state == PENDING paired with the PREVIOUS
 *     s_armed_ms. If that stale timestamp is already >= SEC_CONFIRM_TIMEOUT_MS
 *     in the past, peek() reports TIMEDOUT for a request that was in fact
 *     just armed — a transient, self-correcting display glitch (the next
 *     peek(), after arm()'s stores complete, reads the fresh s_armed_ms and
 *     reports PENDING again). This CANNOT fabricate AUTHORIZED: that return
 *     value depends only on s_state, never on s_armed_ms, and s_state itself
 *     is a single aligned store/load, so peek() never observes a torn
 *     s_state. No grant can be shown, and none can be consumed, that
 *     authorize() did not actually produce. Scope of this guarantee: it
 *     covers peek() racing arm() only; it does NOT claim the analysis above
 *     is otherwise unchanged. Do NOT let a display path call poll().
 *   - sec_confirm_reset() writes the same three fields, in the same order,
 *     as arm() (s_state, then s_slot, then s_armed_ms — see above), and this
 *     comment used to say nothing about it. It's safe for the same reason
 *     peek()-vs-arm() is bounded, plus one more fact specific to reset():
 *     every caller of reset() (dongle_confirm() in ccid.c) runs on the same
 *     task as arm(), so reset() never races arm() itself; and because
 *     reset() writes IDLE first, peek()'s `s_state == PENDING` guard can
 *     never pair a freshly-written state with reset()'s s_armed_ms — a
 *     peek() torn mid-reset() observes either the old PENDING/armed_ms pair
 *     (reset() hasn't stored yet) or IDLE (reset()'s first store already
 *     landed, and IDLE fails the PENDING guard outright), never a PENDING
 *     read paired with a reset() timestamp. */

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
