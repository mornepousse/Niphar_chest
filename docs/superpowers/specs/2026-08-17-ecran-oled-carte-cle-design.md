# The key board's OLED screen — design specification

*2026-08-17*

## Problem

The key board can say **that it is waiting for a finger**: its LED alternates
between the mode's colour and red, and the user presses. Validated on hardware
on 2026-08-17 — without a press, the card returns `6985` and the operation is
refused; with a press, it goes through.

It cannot say **what for**.

That is the structural limit of any key without a display. A malicious host can
request one signature while the user believes they are confirming another: the
press is given in good faith, on an operation nobody chose. The button proves
the presence of a human, never their consent to *this* operation.

The screen fixes exactly that. It turns an acknowledgement of presence into
informed consent, and that is what will make a future FIDO2 defensible: CTAP
requires a presence test, but a key that does not show the requesting site does
not protect against a host that substitutes the request.

Two secondary gains, real but lesser: the key's state becomes readable without
knowing the colour code, and the fifteen-second countdown stops being invisible
— two generations of keys were lost on 2026-08-17 to expiries that nothing
announced.

## Scope

**In scope**: the SSD1306 driver, a display task, the pure logic that decides
the content and the animations, and adding an **operation code** to
`sec_confirm` so the screen can name what it is having confirmed.

**Out of scope**, and deliberately so:

- **the name of the requesting site** — there is no FIDO2. The waiting label is
  a computed string, so the room exists; we build nothing more for a consumer
  that does not exist;
- **TOTP codes** — that is not display but a new function, and it runs into a
  hardware obstacle addressed below;
- **the LED** — it stays, unchanged. See “Division of roles”.

## Hardware

| item | fact |
|---|---|
| Screen | SSD1306, 128×64 monochrome, I²C |
| SCL | **IO53** |
| SDA | **IO54** |

These two pins are safe: neither strapping pin nor restriction. They carry
analog functions (`ADC2_CH4`/`CH5`, analog comparator channel 1) that this
project has no use for — same family as IO51, already taken by the WS2812 LED
without consequence.

> `GPIO53 | ADC2_CH4, ANA_CMPR_CH1 reference voltage | |`
> `GPIO54 | ADC2_CH5, ANA_CMPR_CH1 input (non-inverting) | |`
> — *ESP-IDF Programming Guide*, “GPIO & RTC GPIO — ESP32-P4”, § GPIO Summary
> (empty *Comments* column = no restriction)

**To be checked on the bench before powering up**: the I²C pull-ups. Many
SSD1306 modules carry them on board; adding more puts resistors in parallel.
And the module must be powered from **3.3 V** — an SSD1306 at 5 V with pull-ups
to 5 V would put 5 V back onto pins that do not tolerate it.

## The constraint that governs the architecture

**A full frame blocks for 25 to 30 ms.** 128×64 monochrome = 1024 bytes; at
400 kHz, acknowledge bits included, that is ~23 ms of bus, ~30 ms in practice.

But the HMI task **samples the buttons every 5 ms**, and the debounce
(`main/hmi/button_debounce.h`) assumes regular sampling. A blocking refresh
would eat six sampling periods — hence missed presses on the contact that
carries physical presence. That is not a discomfort: it is the security
mechanism validated the day before.

**Decision: a separate display task.** The buttons keep their cadence, the
screen lives at its own.

The decisive reason is not performance but the countdown bar: it must keep
draining **while** the firmware computes a signature. In a single loop, the
display would freeze exactly at the moment the user looks at the screen to find
out how much time they have left.

Two approaches were rejected:

- **partial refreshes inside the HMI task** (one 8-line band per tick, ~3 ms) —
  elegant, no task and no shared state, but the display stays coupled to the
  button loop and freezes when it freezes;
- **accepting the sampling jitter** — paying for comfort with security.

## Architecture

### Split

| file | nature | responsibility |
|---|---|---|
| `main/hmi/screen_view.h` | **pure** | `(mode, waiting, deadline, verdict, t) →` which screen, which texts |
| `main/hmi/screen_anim.h` | **pure** | anti-burn-in shift, bar fill, slide progress |
| `main/hmi/screen.c` / `.h` | hardware | SSD1306 driver, display task, I²C |

Same rule as the LED, and for the same reason: **no decision in `screen.c`**.
It receives a description of what to paint and it paints. Which screen, what
proportion of bar, which animation phase — all of it is pure and tested on the
host.

The lesson comes from the previous branch: `hmi.c` had promised “no decisions”
and contained four, one of which — the absolute phase of the alternation —
produced a real display defect. What is not extracted is not tested, and what
is not tested drifts.

### State shared between the two tasks

The HMI task publishes a tiny state that the screen task reads:

```c
typedef struct {
    usb_mode_t   mode;
    bool         confirm_pending;
    uint32_t     armed_ms;      /* pour la barre : début de l'attente */
    sec_op_t     op;            /* ce qui est armé — voir plus bas */
    led_event_t  verdict;
    uint32_t     verdict_ms;
} hmi_state_t;
```

**A third context therefore appears**, and the project has a rule about that:
the concurrency reasoning at the top of `main/security/sec_confirm.c`
enumerates the calling contexts and draws a safety conclusion from it. It will
have to be extended — and the screen task must **never** call
`sec_confirm_poll()`, which consumes the authorisation. It reads
`sec_confirm_peek()`, like the HMI task.

### The operation code

The waiting screen has to name what it is having confirmed. But
`sec_confirm_arm()` currently receives only a slot number — an integer with no
description.

**Decision: an operation code, not a string.**

```c
typedef enum {
    SEC_OP_UNKNOWN = 0,
    SEC_OP_SIGN,        /* signature OpenPGP */
    SEC_OP_DECRYPT,
    SEC_OP_AUTH,
    SEC_OP_OTP,
} sec_op_t;
```

`sec_confirm` stays numeric — no text, no allocation, no length bound to defend
in the module that guards the gate. Translating code → label is a **pure**
function in `screen_view.h`, hence testable, and FIDO2 will add its own codes
there without touching the security module.

This is a modification to a module that has already been audited and ported
from `KeSp_firmware`: it must stay minimal and be declared as a divergence.

## The screens

```
AT REST                     AWAITING CONFIRMATION
┌─────────────────────┐     ┌─────────────────────┐
│                     │     │  CONFIRMER ?        │
│      OpenPGP        │     │                     │
│   ───────────────   │     │  Signature OpenPGP  │
│    prête · 3 sign.  │     │                     │
│                     │     │  ███████████░░░░░░  │
└─────────────────────┘     └─────────────────────┘
  shifted every minute        the bar drains in 15 s

VERDICT (600 ms)            MODE SWITCH (400 ms)
┌─────────────────────┐     ┌─────────────────────┐
│                     │     │  OpenPGP  →         │
│       ACCORDÉ       │     │        → Clé OTP    │
│          ✓          │     │                     │
│                     │     │   (slide)           │
└─────────────────────┘     └─────────────────────┘
```

**At rest, the screen shows the mode permanently, shifted by a few pixels every
minute.** OLEDs burn in: static content leaves a permanent mark, and this key
can stay plugged in for days. The shift costs one pure function and removes the
problem.

**The countdown bar is the only animation that is not decorative**: it makes
visible the fifteen seconds of `SEC_CONFIRM_TIMEOUT_MS`, which today are
entirely mute.

## Division of roles with the LED

The LED **stays**, and that is not redundancy: it is seen out of the corner of
the eye, from a distance, without reading. The screen is read, but assumes you
are looking at it.

| | LED | screen |
|---|---|---|
| draw attention | ✅ vivid alternation | ✗ |
| say the mode | colour | name spelled out |
| say **what for** | ✗ | ✅ |
| say the time left | ✗ | ✅ bar |
| verdict | flash | text |

The accepted ambiguity of red — “I am waiting” and “refused”, distinguished by
duration — is **lifted by the screen**: the text says which of the two. The LED
keeps its role of peripheral alert, the screen carries the meaning.

## Handling absences

| situation | behaviour |
|---|---|
| screen absent or I²C mute | `screen_init()` logs and returns an error; the task is not created; **the key stays fully usable**, the LED carries on alone. Same contract as `hmi_init()` today (`main/main.c`) |
| I²C error along the way | log, skip the frame, retry at the next tick; never block an operation |
| verdict arriving during a switch | the verdict wins — it is fleeting and it is what the user is trying to read |
| arming during a switch animation | waiting wins — it is the only screen that demands an action |

## Verification

**On the host, tests written before the implementation:**

- `test_screen_view.c` — the whole mapping (no state without a screen); every
  operation code has a label, and two distinct codes never share one; waiting
  wins over the switch, the verdict wins over the switch.
- `test_screen_anim.c` — the bar is full at arming and empty at the deadline,
  never negative nor beyond 100 %; the computation survives the wraparound of
  the millisecond counter (`uint32_t`, ~49 days); the anti-burn-in shift stays
  within the screen's bounds at any instant.

Every test must **bite**: mutation introduced, red observed, revert.

**On the board**:

- startup: the screen lights up, shows the mode, the key stays mute on USB;
- `gpg` requests a signature: the screen shows “Signature OpenPGP” and the bar
  drains; the press interrupts it and shows “ACCORDÉ”;
- do not press: the bar drains completely, the screen shows the refusal, `gpg`
  fails on `6985`;
- **the bar keeps draining while the firmware computes** — that is the whole
  reason for the separate task, hence the point to check first;
- button sampling stays crisp: no missed press during the animations.

## What this spec does not solve

- **TOTP.** A distinct subsystem, and it runs into a hardware obstacle: the
  board has no battery, so it loses the time at every unplug, whereas TOTP
  requires ±30 s. It would have to resynchronise from the host — and a
  malicious host could then lie about the date and have codes produced for any
  future instant, on a device whose whole point is not to trust the host. To be
  settled before writing anything at all.
- **FIDO2.** The screen is designed to accommodate it, nothing more.
- **Request substitution by a malicious host** is only reduced if the user
  *reads* the screen. A screen that is ignored protects no better than a LED.
  That is an inherent limit, not an implementation defect.

## Risks

| risk | handling |
|---|---|
| Duplicate I²C pull-ups on the module | to be checked on the bench before powering up |
| Burn-in of the panel | periodic shift, tested pure function |
| The screen task makes button sampling drift | separate tasks; to be checked on the bench, not only in theory |
| A third context reads `sec_confirm`'s state | concurrency reasoning to be extended; `peek()` only, never `poll()` |
| Modification of an audited security module | minimal change (one enum, one parameter), declared divergence |
