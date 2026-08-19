#include "sec_confirm.h"

#include <string.h>

/* Frere sans prefixe (main/security/ est a plat sur l'include path — voir
 * main/CMakeLists.txt, INCLUDE_DIRS). Dependance voulue : la taille du
 * tampon s_label ci-dessous et OATH_NAME_DISPLAY_MAX sont le MEME nombre, et
 * les dupliquer les ferait diverger. oath_name.h est pur (aucun appel
 * ESP-IDF), donc ce fichier reste compilable sur l'hote. */
#include "oath_name.h"

#ifndef TEST_HOST
/* Spinlock portMUX, sur le modele exact de security/fido_master.c
 * (s_lock_init_spinlock) : une section critique tres courte (quelques
 * ecritures de mots), jamais une attente bloquante. Gate pour TEST_HOST —
 * voir le bullet sec_confirm_reset() plus bas pour pourquoi ce module en a
 * enfin besoin, et security/sec_store.c pour le meme motif de garde
 * #ifndef TEST_HOST sur un module par ailleurs pur. */
#include "freertos/FreeRTOS.h"
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define SEC_CONFIRM_LOCK()    portENTER_CRITICAL(&s_lock)
#define SEC_CONFIRM_UNLOCK()  portEXIT_CRITICAL(&s_lock)
#else
#define SEC_CONFIRM_LOCK()    ((void)0)
#define SEC_CONFIRM_UNLOCK()  ((void)0)
#endif

/* CONCURRENCY MODEL (Phase-2 pentest, 2026-06-11 ; relu et corrige a l'arrivee
 * de hmi_task, 2026-08-16 ; etendu a s_op, 2026-08-17 ; corrige deux fois en
 * revue le meme jour — d'abord parce que deux lectures separees de l'etat et
 * de l'operation n'etaient PAS couvertes par le raisonnement ci-dessous
 * (voir le bullet sec_confirm_peek_labeled()), ensuite parce que ce bullet
 * lui-meme sur-affirmait : sa premisse invoquait un point de cession
 * cooperatif alors qu'ESP-IDF ordonnance en preemptif, et sa conclusion
 * rangeait le residuel de s_op dans la meme classe "benigne" que celui de
 * s_armed_ms alors que les deux n'appartiennent pas a la meme categorie de
 * risque — les deux corrections sont dans ce meme bullet ; corrigee une
 * TROISIEME fois a la revue finale de branche du plan FIDO2 (2026-08-18, I5)
 * parce que le bullet sec_confirm_reset() plus bas affirmait que reset() ne
 * tourne jamais que sur la meme tache que arm() — vrai avant le correctif I4
 * de usb/usb_mode.c, faux depuis : la fonction de bascule de mode USB
 * (usb/usb_mode.c) appelle desormais sec_confirm_reset() directement, depuis
 * hmi_task ou la tache REPL selon la carte — un contexte distinct de
 * ccid_worker (PGP) et de usb_task (OTP, FIDO). L'ORDRE exact de cet appel
 * vis-a-vis des mode_*_stop()/usb_device_uninstall() est un detail de
 * usb_mode.c, pas de ce fichier, et il a deja bouge une fois sans que la
 * conclusion ci-dessous change : quel que soit cet ordre, ccid_worker ou
 * usb_task peuvent encore etre au milieu d'un arm()/poll() au moment ou
 * reset() s'execute sur son propre contexte. Cette fois la fausse
 * premisse ne pouvait pas se reparer
 * par un argument plus fin comme les trois fois precedentes (peek() contre
 * un seul ecrivain reste bornee, meme torn read) : reset() et arm()/poll()
 * peuvent desormais s'executer EN MEME TEMPS, sur les deux coeurs HP du P4 —
 * deux ECRIVAINS concurrents sur les quatre champs, une classe de course que
 * rien plus bas ne couvrait (tout le raisonnement ci-dessous suppose au plus
 * UN ecrivain actif a la fois). D'ou la QUATRIEME correction, cette fois pas
 * seulement textuelle : le spinlock portMUX ci-dessus serialise desormais
 * arm(), reset(), poll() et authorize() entre eux, fermant la course plutot
 * que de continuer a plaider qu'elle est benigne. peek() reste volontairement
 * HORS verrou — voir son bullet plus bas : c'est un lecteur face a UN SEUL
 * ecrivain a la fois (le verrou garantit que les quatre champs changent
 * desormais comme un groupe) et il ne lit que des entiers, donc son analyse de
 * torn read reste valide sans modification. peek_labeled(), elle, a rejoint le
 * verrou a la CINQUIEME correction (revue finale de branche OATH, I3) : elle
 * copie douze octets d'etiquette, ce qu'aucun argument d'atomicite ne couvre —
 * voir la fin du bullet sec_confirm_peek_labeled() et le corps de la fonction.
 * sec_confirm is a pure module (host-tested, no FreeRTOS). Entry points run
 * from at most THREE task contexts. The three security personalities
 * (OpenPGP CCID, OTP-HID, FIDO U2F) are NOT mutually exclusive AT BUILD —
 * ccid.c, otp_hid.c and u2f.c are all linked into the same binary, every
 * build, on every board. What is actually true, and is what this analysis
 * relies on, is RUNTIME exclusivity: usb/usb_mode.c guarantees that at most
 * one personality's USB descriptors — and, since Task 1 (usb/hid_dispatch.c),
 * at most one HID handlers table — are installed at any instant. Switching
 * personalities always uninstalls the current one (usb_device_uninstall(),
 * which also calls each mode's own *_stop()) before installing the next;
 * a busy flag in usb/usb_mode.c additionally refuses a second mode switch
 * while one is already in flight. Concretely: u2f_handle_apdu()'s
 * arm()/poll() calls (security/u2f.c, reached only from mode_fido.c's
 * handle_message(), itself reached only through the FIDO entry of
 * hid_dispatch.c) can run ONLY while the FIDO table is installed — which is
 * exactly the window during which the OTP table (and therefore
 * otp_hid.c's arm()/poll()) cannot be installed, and vice versa. So although
 * THREE personalities' arm()/poll() code all exist in the binary, at most ONE
 * of {ccid_worker, usb_task-as-OTP, usb_task-as-FIDO} is ever the task
 * actually exercising arm()/poll() at a given moment — the same conclusion
 * the old (wrong) "exclusive at build" premise reached, but for the true
 * reason. This is why the single extra poll context that FIDO's u2f.c adds
 * does NOT trigger the portMUX warning at the end of this comment: it is a
 * new call site, not a new CONCURRENT context — usb_mode.c's runtime
 * exclusivity keeps it disjoint from ccid_worker's and otp_hid.c's, the same
 * way OTP and CCID were already disjoint from each other. A future change
 * that lets two personalities' arm()/poll() run at once (e.g. a background
 * admin path that does not go through usb_mode.c's single-slot install) WOULD
 * still need the portMUX below — the guard is about concurrent EXECUTION, not
 * about how many personalities exist in the link:
 *   - arm() + poll() run on the SAME task (ccid_worker in OpenPGP — see
 *     dongle_confirm() in ccid.c, which also calls reset() — or usb_task
 *     running tud_task_ext() in OTP or FIDO, see otp_hid.c and u2f.c/
 *     mode_fido.c respectively) — serialized with each other by the runtime
 *     exclusivity above, so the (state,slot,op) triple is always written and
 *     read consistently.
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
 *     never a torn or wrong-slot grant. Depuis la correction du Ruling 28,
 *     authorize() LIT aussi s_armed_ms pour verifier que l'appui tombe dans la
 *     fenetre armee. Cette lecture supplementaire ne change pas l'analyse
 *     ci-dessus : s_armed_ms est un mot aligne, donc jamais dechire, et le pire
 *     cas d'une valeur perimee est le meme que celui deja tolere par peek() —
 *     un refus premature, jamais un octroi. La correction est FAIL-SAFE par
 *     construction : toute incertitude sur l'horodatage fait refuser.
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
 * The only race left BETWEEN arm()/poll() of the single active personality
 * and authorize() is the timeout branch at the 15 s boundary; its worst case
 * is a real touch accepted at T+15.0s instead of rejected (or vice-versa) —
 * benign, since the user physically touched for the armed slot (and the
 * caller re-checks the slot). No touch can be fabricated and no wrong slot
 * can be granted. Ce paragraphe ne couvre que arm()/poll() d'UNE SEULE
 * personnalite active a la fois, serialises entre eux par l'exclusivite
 * runtime ci-dessus — pas reset(), qui depuis I4 (voir l'entete CONCURRENCY
 * MODEL) est un ECRIVAIN CONCURRENT distinct de ce raisonnement. C'est pour
 * CETTE course-la, pas celle-ci, que le spinlock portMUX en tete de fichier
 * a ete ajoute : voir le bullet sec_confirm_reset() plus bas.
 *   - peek() is READ-ONLY and adds no writer, but it reads TWO fields
 *     (s_state and s_armed_ms) from a third context — hmi_task, confirmed
 *     real as of main/hmi/hmi.c (this paragraph used to describe a task that
 *     didn't exist yet; it now matches the actual call site) — while arm()
 *     writes them as separate, non-atomic stores (s_state, then s_slot, then
 *     s_op, then s_armed_ms). A peek() interleaved between those stores can
 *     observe a torn snapshot: the new s_state == PENDING paired with the
 *     PREVIOUS s_armed_ms. If that stale timestamp is already >=
 *     SEC_CONFIRM_TIMEOUT_MS in the past, peek() reports TIMEDOUT for a
 *     request that was in fact just armed — a transient, self-correcting
 *     display glitch (the next peek(), after arm()'s stores complete, reads
 *     the fresh s_armed_ms and reports PENDING again). This CANNOT fabricate
 *     AUTHORIZED: that return value depends only on s_state, never on
 *     s_armed_ms, and s_state itself is a single aligned store/load, so
 *     peek() never observes a torn s_state. No grant can be shown, and none
 *     can be consumed, that authorize() did not actually produce. Scope of
 *     this guarantee: it covers peek() racing arm() only; it does NOT claim
 *     the analysis above is otherwise unchanged. Do NOT let a display path
 *     call poll().
 *   - sec_confirm_peek_labeled() is READ-ONLY and reads s_op alongside
 *     s_state/s_armed_ms from the same third context (hmi_task, for the
 *     operation label). CONTRACT, not a suggestion: a caller that wants both
 *     the state and the label MUST call this one function, never peek() and
 *     a separate op read side by side. Two separate calls have an UNBOUNDED
 *     window between them — nothing serializes hmi_task's calls against
 *     ccid_worker, so an entire reset()+arm() cycle (a whole NEW operation)
 *     can complete in that window. The reader would then pair the fresh
 *     state of operation B with the stale label of operation A, read a whole
 *     call earlier — not a torn store, a torn STORY. That is not a display
 *     glitch: authorize() only checks s_state, so a user who confirms
 *     because the screen names A would in fact grant B. This is exactly the
 *     attack the screen exists to close, so it may not reopen it through the
 *     accessor design. Bundling into one function does not make the read
 *     atomic — s_op and s_state are still two separate loads inside
 *     peek_labeled() — but it shrinks the window from "arbitrary
 *     caller-scheduled work between two API calls" to "a few CPU cycles
 *     between two adjacent instructions". This is a PROBABILITY argument,
 *     not an ordering guarantee: FreeRTOS on this target runs preemptive
 *     (configUSE_PREEMPTION=1), so a tick interrupt can land between ANY two
 *     instructions, including these two loads — there is no such thing as a
 *     cooperative gap to avoid landing in. What changed is not whether
 *     preemption can happen here, but how much CPU time the window spans:
 *     a handful of cycles inside one function body versus however long the
 *     scheduler leaves hmi_task off-core between two separate calls (which
 *     can be milliseconds). s_op is read FIRST, on entry, before the timeout
 *     check, purely to keep the two loads textually adjacent and minimise
 *     that cycle count — it does not close the window, only narrows it.
 *     A residual tear is still possible in principle (arm() interleaving
 *     exactly between the two loads, during a live tick) and is transient
 *     and self-correcting the same way as the s_armed_ms tear above (the
 *     very next peek_labeled() call, once arm() has finished, reads the
 *     fresh pair) — but it is NOT in the same risk CATEGORY, and must not be
 *     filed under the same "benign" label without that caveat: the
 *     s_armed_ms tear is fail-safe by construction (it can only make peek()
 *     report TIMEDOUT too early — refuse sooner, never grant later or grant
 *     wrong), whereas a residual s_op tear can display operation A's label
 *     while the state shown actually belongs to operation B — the exact
 *     mis-attribution this whole feature exists to prevent, reproduced at
 *     vanishing probability instead of eliminated. It still CANNOT fabricate
 *     or misattribute AUTHORIZED itself — that continues to depend only on
 *     s_state via poll(), which peek_labeled() never touches, so no wrong
 *     GRANT can result, only a wrong LABEL for the width of one tick — but a
 *     wrong label is exactly the surface this feature is built to close, so
 *     "probability comparable to an already-accepted tear" is not the same
 *     claim as "equally safe by construction". Deliberately NOT closed with
 *     a portMUX critical section: sec_confirm stays a pure, host-tested
 *     module with no FreeRTOS dependency, which is what makes its state
 *     machine testable without hardware, and the residual requires a tick
 *     interrupt to land inside a two-instruction window WHILE a concurrent
 *     reset()+arm() is in flight — improbable, and not something an
 *     attacker can force from outside (they do not control ESP-IDF's tick
 *     scheduling). Revisit only if this residual is ever shown to be
 *     steerable, not just theoretically present.
 *     ETENDU (Tache 5, ronde de revue 1) : s_label (l'etiquette de compte
 *     OATH) a rejoint s_op dans CE meme corps de fonction, lu juste a cote,
 *     pour la raison EXACTE ci-dessus — un accesseur sec_confirm_label()
 *     separe avait d'abord ete ecrit, puis retire en revue : un appelant qui
 *     enchainerait peek_labeled() puis cet accesseur separe au site d'appel
 *     rouvrirait la fenetre non bornee que ce bullet existe pour fermer,
 *     tenue seulement par la discipline « rien entre les deux appels », que
 *     rien ne verifie sur un fichier qui n'entre jamais dans le harnais hote
 *     (hmi.c, seul appelant, est derriere #if BOARD_CONFIRM_SOURCE). Le
 *     residu sur s_label est de meme nature que celui sur s_op ci-dessus :
 *     transitoire, auto-corrige au tick suivant, jamais un octroi errone.
 *
 *     CE DERNIER PARAGRAPHE ETAIT FAUX, et corrige a la revue finale de
 *     branche OATH (I3) : le residu sur s_label n'est PAS de meme nature que
 *     celui sur s_op. s_op est un entier, donc un seul chargement aligne : un
 *     lecteur ne peut en tirer qu'une valeur perimee, jamais une valeur qui
 *     n'a jamais ete ecrite. s_label est un memcpy de douze octets, que rien
 *     ne rend atomique : un arm() concurrent peut atterrir en son milieu et
 *     le lecteur repartir avec la moitie d'une etiquette collee a la moitie
 *     d'une autre — un nom de compte que personne n'a jamais arme. Un label
 *     PERIME est deja le defaut que ce bullet decrit ; un label FABRIQUE en
 *     est un autre, et il ne se corrige pas en plaidant la probabilite.
 *     sec_confirm_peek_labeled() prend donc desormais le verrou (voir son
 *     corps) : elle n'est appelee qu'une fois par tick IHM, le cout est un
 *     memcpy de douze octets sous portMUX. Tout ce bullet decrit donc
 *     l'histoire d'une decision, pas l'etat actuel du code — seul
 *     sec_confirm_peek(), qui ne lit que des entiers, reste hors verrou.
 *   - sec_confirm_reset() writes the same four fields, in the same order, as
 *     arm() (s_state, then s_slot, then s_op, then s_armed_ms — see above).
 *     CE QUI SUIT ETAIT FAUX (corrige a la revue finale de branche du plan
 *     FIDO2, 2026-08-18, I5) : une version precedente affirmait ici que
 *     « every caller of reset() (dongle_confirm() in ccid.c) runs on the
 *     same task as arm(), so reset() never races arm() itself ». C'etait
 *     exact tant que dongle_confirm() etait le SEUL appelant. Depuis le
 *     correctif I4 de usb/usb_mode.c, un second appelant existe : la
 *     fonction de bascule de mode USB y appelle sec_confirm_reset()
 *     directement, depuis hmi_task ou la tache REPL — jamais depuis
 *     ccid_worker (PGP) ni usb_task (OTP, FIDO), quel que soit l'ordre exact
 *     de cet appel vis-a-vis des mode_*_stop()/usb_device_uninstall() (un
 *     detail de usb_mode.c qui a deja bouge une fois, voir son historique,
 *     sans que ce paragraphe ait besoin de changer). Ce qui compte ici
 *     n'est pas CET ordre mais le fait que ce second appelant tourne sur une
 *     tache differente de celle qui execute arm()/poll() pour la
 *     personnalite qu'on est en train de quitter — elle peut donc encore
 *     etre au milieu d'un arm()/poll() sur SA propre tache au moment ou
 *     reset() s'execute. Aucune de ces taches n'est hmi_task ni la tache
 *     REPL : la premisse ne pouvait donc plus etre vraie pour aucune des
 *     trois personnalites, pas seulement OTP/FIDO comme un premier examen le
 *     suggerait — le fait que dongle_confirm() se re-teste lui-meme contre
 *     un drapeau d'arret (s_shutdown, voir ccid_shutdown()) borne la fenetre
 *     reelle pour PGP mais ne change rien a la premisse ELLE-MEME, qui reste
 *     fausse.
 *
 *     Le raisonnement « premier champ pose la visibilite » qui suivait
 *     (reset() ecrit IDLE en premier, donc un peek() dechire ne peut jamais
 *     lire un s_state frais avec un s_armed_ms perime) supposait un seul
 *     ecrivain actif a la fois entre reset() et arm(). Avec deux ecrivains
 *     VRAIMENT concurrents (deux coeurs HP, aucune synchronisation), les
 *     HUIT ecritures de reset() et arm() peuvent s'entrelacer champ par
 *     champ dans n'importe quel ordre : le quadruplet final peut melanger,
 *     par exemple, le s_state de arm() (PENDING) avec le s_armed_ms de
 *     reset() (0) — poll()/authorize() y voient alors une operation perimee
 *     depuis l'epoque Unix, donc refusee (fail-safe pour CE cas precis) —
 *     mais l'entrelacement inverse existe aussi : s_state=PENDING (de arm(),
 *     landed last) avec s_slot=0 (de reset(), landed last APRES le s_slot de
 *     arm()). Si un appui physique REEL survient pendant cette fenetre,
 *     authorize() accorde legitimement AUTHORIZED, et poll() rend le slot 0
 *     — pas le slot que arm() avait reellement arme. Pour l'OTP, dont les
 *     index de sec_store valent precisement 0 et 1 (otp_hid.c), ce n'est pas
 *     un cas d'ecole : un octroi peut alors s'appliquer au MAUVAIS slot
 *     HMAC. C'est une classe d'erreur differente de tout ce que ce fichier
 *     tolerait jusqu'ici — pas un refus premature (fail-safe), mais un octroi
 *     mal attribue sur un geste physique reel. D'ou le spinlock portMUX en
 *     tete de fichier : arm(), reset(), poll() et authorize() s'executent
 *     desormais chacun sous verrou, donc les quatre champs changent toujours
 *     comme un groupe indivisible — plus aucun entrelacement champ par champ
 *     n'est possible entre ecrivains. peek()/peek_labeled() restent hors
 *     verrou : ce sont des lecteurs qui ne font face qu'a UN SEUL ecrivain a
 *     la fois maintenant (le verrou a elimine le second), donc leur analyse
 *     de torn read ci-dessus reste valide sans aucune modification.
 *
 *     poll() clears s_op to SEC_OP_UNKNOWN on both its consuming branches
 *     (AUTHORIZED and TIMEDOUT), symmetric with reset(): a field that
 *     survives the consumption of the operation it describes is a question
 *     the next reader would have to re-derive the answer to, so the
 *     invariant "IDLE implies SEC_OP_UNKNOWN" holds after every path that
 *     reaches IDLE, not just reset(). */

static sec_confirm_state_t s_state    = SEC_CONFIRM_IDLE;
static uint8_t             s_slot     = 0;
static uint32_t            s_armed_ms = 0;
static sec_op_t            s_op       = SEC_OP_UNKNOWN;
/* Etiquette affichable de l'operation armee. Toujours une chaine terminee,
 * jamais NULL : screen.c la passe a draw_text_2x_centered()/draw_text_centered()
 * sans garde. Videe par reset(), comme s_op — meme raison : un champ qui
 * survit a la consommation de l'operation qu'il nomme est une reponse que le
 * prochain lecteur devrait re-deriver. */
static char s_label[OATH_NAME_DISPLAY_MAX] = { 0 };

void sec_confirm_reset(void)
{
    /* Sous verrou : voir le bullet sec_confirm_reset() de l'entete
     * CONCURRENCY MODEL — depuis I4 (usb/usb_mode.c), cette fonction peut
     * tourner en meme temps que arm()/poll() sur une tache differente. */
    SEC_CONFIRM_LOCK();
    s_state    = SEC_CONFIRM_IDLE;
    s_slot     = 0;
    s_op       = SEC_OP_UNKNOWN;
    s_armed_ms = 0;
    s_label[0] = '\0';
    SEC_CONFIRM_UNLOCK();
}

void sec_confirm_arm(uint8_t slot, sec_op_t op, uint32_t now_ms)
{
    sec_confirm_arm_named(slot, op, NULL, now_ms);
}

void sec_confirm_arm_named(uint8_t slot, sec_op_t op, const char *label, uint32_t now_ms)
{
    /* Assainissement HORS verrou : oath_name_display() est pure et n'a rien a
     * faire dans une section critique deja tres courte. Le resultat local
     * est ensuite recopie sous verrou, avec les trois autres champs, pour
     * que les cinq changent comme un seul groupe indivisible — meme
     * discipline que le reste de arm()/reset() (voir CONCURRENCY MODEL). */
    char formatted[OATH_NAME_DISPLAY_MAX];
    /* Mis a zero AVANT le formatage, et pas seulement termine apres :
     * oath_name_display() n'ecrit que jusqu'au terminateur, or le memcpy
     * ci-dessous en recopie les DOUZE octets. Sans ce memset, des octets de
     * pile non initialises entrent dans un statique que l'ecran dessine — et
     * dans le memcmp de snap_differs() (hmi/screen.c), qui compare
     * l'etiquette sur toute sa longueur : deux armements du meme compte
     * pouvaient differer par de la pile, d'ou un redessin fantome. Corrige a
     * la revue finale de branche OATH (m1). */
    memset(formatted, 0, sizeof(formatted));
    oath_name_display(label, label ? (uint16_t)strlen(label) : 0u,
                      formatted, sizeof(formatted));

    SEC_CONFIRM_LOCK();
    s_state    = SEC_CONFIRM_PENDING;
    s_slot     = slot;
    s_op       = op;
    s_armed_ms = now_ms;
    memcpy(s_label, formatted, sizeof(s_label));
    SEC_CONFIRM_UNLOCK();
}

/*
 * L'appui doit tomber DANS la fenetre de l'operation actuellement armee.
 *
 * Le defaut que ceci ferme (Ruling 28) : authorize() ne prenait aucun
 * horodatage, donc il ne pouvait pas savoir QUAND le geste avait eu lieu. Il ne
 * voyait qu'un s_state a PENDING et accordait.
 *
 * Le scenario est reel, pas theorique. hmi_task lit peek() puis appelle
 * authorize() : deux appels separes, que rien ne serialise contre la tache qui
 * arme. Si l'ordonnanceur la laisse hors CPU entre les deux — des
 * millisecondes, sous charge — l'operation A peut expirer et une operation B
 * s'armer dans l'intervalle. L'appui destine a A autorisait alors B, et
 * l'utilisateur avait physiquement confirme quelque chose qu'on ne lui avait
 * jamais montre. C'est exactement l'attaque que cette porte existe pour fermer.
 *
 * Theorique sur OpenPGP, ou les operations arrivent une par une ; reel des
 * qu'un navigateur enchaine des getAssertion FIDO. Et sans PIN dans le
 * perimetre FIDO retenu, cette porte est la SEULE defense qui reste.
 *
 * UNE condition ferme les deux cotes de la fenetre, et c'est la meme expression
 * que poll() et peek() : un appui anterieur a l'armement donne une difference
 * non signee enorme (elle repasse par le haut), donc >= au delai — refuse. Un
 * appui posterieur au delai est refuse par la meme comparaison. Ecrire deux
 * tests separes aurait invite a une divergence ; ici il n'y a rien a faire
 * diverger.
 */
void sec_confirm_authorize(uint32_t pressed_at_ms)
{
    SEC_CONFIRM_LOCK();
    if (s_state != SEC_CONFIRM_PENDING) {
        SEC_CONFIRM_UNLOCK();
        return;
    }
    if ((uint32_t)(pressed_at_ms - s_armed_ms) >= SEC_CONFIRM_TIMEOUT_MS) {
        SEC_CONFIRM_UNLOCK();
        return;
    }
    s_state = SEC_CONFIRM_AUTHORIZED;
    SEC_CONFIRM_UNLOCK();
}

sec_confirm_state_t sec_confirm_poll(uint32_t now_ms, uint8_t *out_slot)
{
    SEC_CONFIRM_LOCK();
    if (s_state == SEC_CONFIRM_PENDING &&
        (now_ms - s_armed_ms) >= SEC_CONFIRM_TIMEOUT_MS) {
        s_state = SEC_CONFIRM_IDLE;
        s_op    = SEC_OP_UNKNOWN;
        /* L'etiquette part avec l'operation qu'elle nomme, sur les DEUX
         * chemins de sortie. La laisser derriere republierait a chaque tick de
         * l'IHM le nom d'un compte que plus rien n'attend. Corrige a la
         * re-revue de la tache 6 : le champ etait vide par reset() et par
         * arm(), jamais par poll() — alors que son commentaire de definition
         * affirmait le contraire. */
        s_label[0] = '\0';
        SEC_CONFIRM_UNLOCK();
        return SEC_CONFIRM_TIMEDOUT;
    }
    if (s_state == SEC_CONFIRM_AUTHORIZED) {
        if (out_slot) *out_slot = s_slot;
        s_state = SEC_CONFIRM_IDLE;
        s_op    = SEC_OP_UNKNOWN;
        s_label[0] = '\0';   /* meme raison qu'au chemin d'expiration ci-dessus */
        SEC_CONFIRM_UNLOCK();
        return SEC_CONFIRM_AUTHORIZED;
    }
    sec_confirm_state_t st = s_state;
    SEC_CONFIRM_UNLOCK();
    return st;
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

sec_confirm_state_t sec_confirm_peek_labeled(uint32_t now_ms, sec_op_t *out_op, char *out_label)
{
    /*
     * SOUS VERROU — corrige a la revue finale de branche OATH (I3).
     *
     * Ce qui etait faux : l'etiquette etait copiee HORS verrou, avec le meme
     * argument que pour s_op — « lecture adjacente, fenetre bornee a quelques
     * cycles, residu transitoire et auto-corrige ». L'argument ne se transpose
     * PAS. s_op est un entier : sa lecture est un seul chargement aligne, donc
     * jamais dechiree — le residu ne peut donner qu'une valeur perimee, jamais
     * une valeur qui n'a jamais existe. s_label est un memcpy de douze octets,
     * que rien ne rend atomique : arm() peut atterrir en plein milieu, et le
     * lecteur reparte avec la MOITIE d'une etiquette collee a la moitie d'une
     * autre. L'ecran nommerait alors un compte qui n'existe pas, pendant
     * qu'un autre est vise — exactement ce que la decision 4 de la spec
     * existe pour empecher, et pas seulement « un label perime ».
     *
     * Le cout est nul a l'echelle ou cette fonction vit : hmi.c l'appelle une
     * fois par tick IHM, et la section critique se reduit a un memcpy de douze
     * octets et deux comparaisons. Prendre le verrou ferme AUSSI, en prime, le
     * residu sur s_op decrit au CONCURRENCY MODEL : les cinq champs sont
     * desormais lus comme un groupe indivisible, du meme cote du verrou que
     * arm(), reset() et poll() les ecrivent.
     *
     * sec_confirm_peek() (sans etiquette) reste volontairement hors verrou :
     * elle ne lit que des entiers, son analyse de torn read tient, et elle est
     * sur le chemin de tick d'appelants qui n'ont pas besoin du nom.
     */
    SEC_CONFIRM_LOCK();
    if (out_op) *out_op = s_op;
    if (out_label) memcpy(out_label, s_label, sizeof(s_label));
    const int expire = (s_state == SEC_CONFIRM_PENDING &&
                        (now_ms - s_armed_ms) >= SEC_CONFIRM_TIMEOUT_MS);
    const sec_confirm_state_t st = s_state;
    SEC_CONFIRM_UNLOCK();
    return expire ? SEC_CONFIRM_TIMEDOUT : st;
}
