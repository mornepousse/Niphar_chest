# WT9932 key board — design specification

*2026-08-16*

## Problem

The project has two boards: the dev kit, where confirmation comes from a
console command with no security value whatsoever, and the chest, where it will
come from the keyboard over an SPI link that does not yet exist. Neither of
them currently makes it possible to exercise the full chain — *the host asks
for a signature, a human touches a contact, the operation goes through* —
because neither has a contact.

The WT9932P4-TINY board changes that. It is neither a kit nor a chest: it is a
**third product variant**, a standalone security key, which does with its own
buttons what the integrated variant will do with the keyboard. Both are
products; neither is the other's crutch.

This spec covers the board, its local HMI, and the rework of the board
abstraction that its mere existence makes necessary.

## Scope

**In scope**: the board file, an HMI subsystem (two buttons, one addressable
LED), separating the board flags, disabling the microSD, and wiring the
confirmation button to `sec_confirm`.

**Out of scope**: the S3 link (task #9, unchanged), FIDO/CTAP, and any
modification to the OpenPGP or OTP stack — this board runs them as they are.

## Hardware — what is established, and by what

Board **WT9932P4-TINY_1v2**, module **WT0132P4-A1** (ESP32-P4 rev v1.0, 32 MB
PSRAM, 16 MB flash). Source: the manufacturer schematic
`Schematic-ESP32P4-TINY-WT0132P4-A1-Pocket-Development-Board`, JLCEDA V1.0,
revised 2025-08-07.

| item | fact | source |
|---|---|---|
| Addressable LED | `DIN ← IO51`, `VDD ← 5 V`, 100 nF decoupling | schematic, LED block |
| Indicator LED | plain LED on R13 1 kΩ, not software-driven | schematic, LED block |
| BOOT button | SW2 → **IO35**, R4 10 kΩ to 3.3 V, C8 100 nF | schematic, KEY block |
| RESET button | SW1 → CHIP_PU, R1 10 kΩ, C1 100 nF | schematic, KEY block |
| USB OTG HS | J4 → inductor L3 → `USB_DP`/`USB_DM` (dedicated PHY pins) | schematic, USB block |
| USB-Serial-JTAG | J3 → inductor L2 → `IO25`/`IO24` | schematic, USB串口 block |
| microSD | **absent** — no connector on the schematic | schematic, page 1/2 |
| Free pins | IO26–IO33 broken out on J7 | schematic, connector J7 |

Verified on hardware on 2026-08-16: 32 MB PSRAM at 200 MHz in X16 mode
(`esp_psram: SPI SRAM memory test OK`, 32,320 K on the heap),
`gpg --card-status` answers on the OTG port, and the
`none → pgp → otp → storage → none` cycle passes.

### IO35 is not used, and that is deliberate

IO35 is the P4's boot-mode strapping pin — the equivalent of IO0 on the
ESP32-S3, and the manufacturer put the BOOT button there for that reason.

> “ESP32-P4 has five strapping pins: GPIO34, GPIO35, GPIO36, GPIO37, GPIO38”
> — *ESP32-P4 TRM*, ch. 11 “Chip Boot Control”, p. 795
>
> Table 11.2-2 — `SPI Boot mode (default) : GPIO35 = 1` ·
> `Joint Download Boot mode : GPIO35 = 0, GPIO36 = 1`
> — *ibid.*, p. 796

The silicon does allow it to be used: “After the reset is released, the
strapping pins work as normal-function pins” (*ibid.*, §11.2.1). We forgo it
anyway, for three cumulative reasons:

1. **A press during power-up prevents the key from booting** — it goes into
   download mode. A reset that happens with the button held (brownout,
   watchdog) does the same, and the key vanishes from the bus with no
   explanation.
2. **Guardrail no. 1 of `scripts/fast.sh` would stay green.** It already
   excludes `boards/*/board.h` from its grep — “those are the files that
   declare them reserved”. Declaring `BOARD_BUTTON GPIO_NUM_35` would therefore
   pass silently, while the meaning of GPIO35 would be inverted there: from
   *reserved* to *user button*. The guard watches the name, not the intent.
3. **The boot margin is narrow.** C8 (100 nF) charges through R4 (10 kΩ), i.e.
   τ ≈ 1 ms on a pin sampled at reset. What saves the board is the identical RC
   on CHIP_PU, which holds the chip back long enough for IO35 to rise. It works
   — verified a dozen times — but it is a matching of time constants, not a
   guarantee.

The buttons therefore go on **IO32** and **IO33**, wired by the user to ground,
internal pull-up enabled in firmware. Together with their neighbours IO26–31,
they are the only P4 pins whose three columns in the GPIO table are empty:
no analog function, no LP GPIO, no restriction.

> `GPIO26 | | |` … `GPIO33 | | |`
> — *ESP-IDF Programming Guide*, “GPIO & RTC GPIO — ESP32-P4”, § GPIO Summary

They are also outside the two blocks occupied on the sister boards — SD on
39–48, S3 link on 7–11 — hence with no conceptual collision.

## Architecture

### 1. Separate what `BOARD_LINK_AVAILABLE` was conflating

That flag currently answers two questions at once: “is there an SPI link?” and
“is the console crutch allowed?”. The conflation held as long as there were
only two boards. The third breaks it: no link, a button, and the console kept.

```c
/* main/board_common.h */
#define BOARD_CONFIRM_NONE    0   /* aucune source réelle de présence */
#define BOARD_CONFIRM_LINK    1   /* le S3, par le lien SPI */
#define BOARD_CONFIRM_BUTTON  2   /* un bouton en façade */
```

Three questions, three flags:

| board | `BOARD_CONFIRM_SOURCE` | `BOARD_LINK_AVAILABLE` | `BOARD_CONSOLE_ACTIONS` | `BOARD_HAS_SD` |
|---|---|---|---|---|
| `jc_devkit` | `NONE` | 0 | 1 | 1 |
| `niphar_chest` | `LINK` | 1 | **0** | 1 |
| `wt9932_key` | `BUTTON` | 0 | 1 | **0** |

`BOARD_LINK_AVAILABLE` regains a single, literal meaning. Guardrail no. 4 of
`fast.sh` stops referring to it and relies on `BOARD_CONSOLE_ACTIONS`, which is
the question it actually asks. Three `_Static_assert` in `board_common.h` lock
down the consistency:

```c
_Static_assert(BOARD_CONFIRM_SOURCE == BOARD_CONFIRM_NONE
            || BOARD_CONFIRM_SOURCE == BOARD_CONFIRM_LINK
            || BOARD_CONFIRM_SOURCE == BOARD_CONFIRM_BUTTON,
    "BOARD_CONFIRM_SOURCE n'a pas une des trois valeurs connues");

#if BOARD_CONFIRM_SOURCE == BOARD_CONFIRM_LINK
_Static_assert(BOARD_LINK_AVAILABLE,
    "présence annoncée par le lien sur une carte qui n'en a pas");
#endif

/* `defined()` n'existe pas dans une expression C : ce troisième contrôle se
 * fait au préprocesseur, pas en _Static_assert. */
#if BOARD_CONFIRM_SOURCE != BOARD_CONFIRM_BUTTON && defined(BOARD_BTN_CONFIRM)
#error "bouton de confirmation déclaré sur une carte dont ce n'est pas la source"
#endif
```

### 2. `BOARD_HAS_SD` — without which the key takes eleven seconds to start

`board_common.h` currently defines the SD pinout unconditionally and
`main.c:58-62` probes at startup. On a board with no connector, the probe fails
by timeout, twice (external path then LDO), and costs **about eleven seconds**
— measured on this module on 2026-08-16:

```
E (1861)  sdmmc_periph: sdmmc_host_clock_update_command … returned 0x107
E (10861) sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned 0x107
W (10891) sd: aucune carte (ESP_ERR_TIMEOUT) — le coffre reste utilisable, la SD non
```

Eleven seconds before a security key responds to its first press, to look for a
component that is not soldered on. `BOARD_HAS_SD 0` removes the startup probe
and the `sd` console command — not the pinout.

**Decided otherwise during the branch, and it is the delivered code that is
authoritative here**: `main/usb/msc_disk.c` calls
`sd_present()`/`sd_read_sectors()` unconditionally, on all three boards — that
is what serves the `storage` mode of the USB cycle. For that to compile,
`sd_card.c` must compile on all three boards, so the microSD pinout in
`board_common.h` (six `_Static_assert`) stays **unconditional**: neither those
blocks nor `sd_card.c` go under `#if BOARD_HAS_SD`. That flag governs only two
things, both in files other than `board_common.h`: the startup probe (`main.c`)
and the `sd` console command (`console.c`). The `storage` mode of the key
board's cycle, for its part, does not depend on `BOARD_HAS_SD` — it is absent
from the cycle by construction anyway (`usb/usb_mode_cycle.h`: the key has only
two steps, `pgp` and `otp`), independently of the presence of a microSD
connector. See `.tripwire-divergences` for the corresponding divergence,
already declared.

### 3. The `main/hmi/` subsystem

Split according to the project's TDD norm: what computes is pure and tested on
the host, what touches hardware is thin and untested.

| file | nature | responsibility |
|---|---|---|
| `hmi/button_debounce.h` | **pure** | filters a raw level into stable edges |
| `hmi/led_state.h` | **pure** | `(mode, waiting, verdict) → (colour, alt colour, pattern)` |
| `usb/usb_mode_cycle.h` | **pure** | `none → pgp`, then `pgp ⇄ otp` |
| `hmi/hmi.c` | hardware | input GPIOs, `led_strip` over RMT, the task |

**Two buttons, one job each.** IO32 switches the mode, IO33 confirms. No
duration threshold, hence no ambiguity: a press is one action, and nothing
else. That is the underlying reason for the choice, not a convenience. With a
single button distinguishing long and short presses, the time threshold would
be the only thing separating “I confirm this signature” from “I uninstall the
CCID while the host is using it” — a finger that lingers would tear the
interface away mid-operation. A physical presence gate must do one thing.

**The long press no longer exists, and `usb_mode_set()` stays confined.**
Guardrail no. 4 restricts that symbol to `usb_mode.c` and `console.c`; widening
it to `hmi.c` would weaken the guard for no gain. `usb_mode.c` therefore
exposes `usb_mode_cycle_next()`, and the cycle policy stays with the module
that owns the modes.

### 4. `sec_confirm_peek()` — without which the LED steals signatures

The LED has to signal when an operation is armed, so the HMI task must know
`sec_confirm`'s state. But `sec_confirm_poll()` **consumes** the permission:

> “AUTHORIZED -> writes slot to *out_slot, consumes (-> IDLE), returns AUTHORIZED”
> — `main/security/sec_confirm.h:20-21`

A display task calling `poll()` would steal the permission from whoever is
waiting for it, and the signature would fail with no trace. A side-effect-free
read is therefore needed:

```c
/* Lit l'état sans rien consommer ni expirer. Pour l'affichage seulement :
 * seul poll() fait avancer la machine. */
sec_confirm_state_t sec_confirm_peek(uint32_t now_ms);
```

Pure, with no mutated state, testable along with the rest of `sec_confirm`.
`now_ms` is there to report an expiry that has already been reached without
consuming it — the LED must be able to show the refusal.

## Behaviour

```
plug in ──────▶ [none]  LED off, nothing on the bus
                   │
                   │ MODE press (IO32)
                   ▼
              [pgp] ●blue ◀── MODE press ──▶ [otp] ●green

operation armed      ●blue/●red (or ●green/●red in otp) — alternating
                       full brightness, 1 Hz, sharp (no fade)
CONFIRM press (IO33) ☀ white flash 120 ms — ONLY if an operation
                       was armed → sec_confirm_authorize()
refusal / expiry     ☀ red flash 120 ms
mode switch          ☀ flash of the new colour, 120 ms
```

**Revision 2026-08-17, after real use of the board.** Waiting for confirmation
originally pulsed in brightness (0 → `LED_DIM` on the mode's colour). Tried on
hardware, that pulsing proved too discreet: you miss it unless you stare at the
LED, and a physical presence gate you cannot see opening makes you miss
signatures. Waiting now alternates sharply between the mode's colour and red,
both at `LED_BRIGHT` — `led_state_view()` exposes that second term via
`rgb_alt`, and `hmi.c` switches between `rgb` and `rgb_alt` without ever
knowing what they mean.

**Accepted reservation.** Red now carries two opposite meanings — “refused” on
the 120 ms flash, “waiting” on the 1 Hz alternation. This is not an accidental
ambiguity: it is a deliberate trade-off, where duration is the only
distinguishing sign (120 ms against 15 s, `SEC_CONFIRM_TIMEOUT_MS`). Adopted
despite the warning, because missing a confirmation window costs more than a
second of attentive reading of the signal's duration.

Consistent with the project's principle: **nothing is exposed at startup**. The
key arrives mute and the first MODE press arms it. `none` is no longer reached
afterwards without unplugging — an accepted decision: two steps are better than
three in use, and unplugging a key is a natural gesture.

Software debounce of 20 ms, on the falling edge (buttons active low). The
user's buttons have no RC: filtering is entirely in software, unlike SW2 which
has one.

Low brightness at rest — 20/255 on all three channels — and 120/255 on the
verdict flashes and on the waiting alternation, which must be visible. It is a
key, not a lamp.

## Handling absences

| situation | behaviour |
|---|---|
| `usb_mode_cycle_next()` fails | mode kept, `known = false` (existing mechanism), red flash |
| CONFIRM press with no operation armed | `sec_confirm_authorize()` is already a no-op; no flash, so as not to suggest that something happened |
| LED absent or mute | `hmi.c` logs and carries on; the absence of a display never blocks an operation |
| bounce on MODE during a signature | the mode switches, the CCID is uninstalled — that is the requested behaviour, and debouncing is its only protection |

## Verification

**On the host, with no hardware**, tests written before the implementation:

- `test_button_debounce.c` — bounce on the edge, chained presses, long hold
  (which must produce **only one** edge), release during the bounce.
- `test_led_state.c` — the whole mapping: no state without a colour, no colour
  shared by two modes, the alternation is only produced while waiting, its two
  colours differ and are visible, `rgb_alt` equals `rgb` outside waiting.
- `test_usb_mode_cycle.c` — `none → pgp`, `pgp → otp`, `otp → pgp`, and `none`
  never returned after the first call.
- `test_sec_confirm.c` (existing, extended) — `peek()` does not consume: a
  `peek` followed by a `poll` still returns `AUTHORIZED`.

Every test must bite: transient bug introduced, red observed, revert.

**On the board**:

- mute startup, no USB device, LED off;
- MODE press → CCID enumerates, `gpg --card-status` answers, LED blue;
- `gpg --card-status` then a signature → blue/red alternation, CONFIRM press,
  white flash, signature produced;
- do not press → red flash at 15 s (`SEC_CONFIRM_TIMEOUT_MS`), `gpg` fails;
- MODE press → HID enumerates, LED green;
- time from startup to the LED: **under one second** (checks `BOARD_HAS_SD`).

**On the sister boards**, non-regression: `jc_devkit` and `niphar_chest` build
and behave as before. `sec_gate_console_confirm` stays absent from the
`niphar_chest` binary (existing `nm` check).

## What this board will not prove

It has to be written down, otherwise we will convince ourselves we validated
more than that.

- **The S3 link**: absent from all three boards. The integrated variant remains
  unproven.
- **The CR-HMAC exchange**: no HID tooling on the development machine. OTP mode
  is only verified as far as the kernel binding.
- **Resistance to a flash dump**: the board has a BOOT button on the front and
  a RESET button. Anyone with the board in hand enters download mode and reads
  the flash — which contains the OpenPGP private keys. The countermeasure
  planned for the production board (JTAG jumpers removed in manufacturing, cf.
  `docs/HARDWARE.md`) does not apply here.
- **The chest's unrecoverability constraints**: this board forgives, like the
  kit. It exercises a security *behaviour*, not a hardware posture.

## Risks

| risk | scope | handling |
|---|---|---|
| 5 V LED driven by 3.3 V data | a strict WS2812 requires VIH ≥ 0.7 × VDD = 3.5 V; we are below | to be observed on the bench. Wrong colours or flicker = hardware cause, not software. Workaround if needed: power the LED from 3.3 V |
| Pins read off a rendering of the schematic | J7 and the button pinout | to be cross-checked against the silkscreen before soldering |
| `BOARD_HAS_SD` governs the startup probe and the `sd` console command | affects all three boards (the pinout itself stays unconditional in `board_common.h`) | the two existing boards keep `1`; non-regression covered by the full `check.sh` |
| The flag rework touches `fast.sh` | guardrail no. 4 | guard rewritten on `BOARD_CONSOLE_ACTIONS`, and its effectiveness re-proven by mutation |

## Divergences to declare

To be recorded in `.tripwire-divergences` at the moment of introduction:

1. `fast.sh` — guardrail no. 4 on `BOARD_CONSOLE_ACTIONS` and no longer
   `BOARD_LINK_AVAILABLE`: the watched pattern changes name.
2. `board_common.h` — the SD pinout stays **unconditional** despite the arrival
   of `BOARD_HAS_SD`: `main/usb/msc_disk.c` calls
   `sd_present()`/`sd_read_sectors()` unconditionally on all three boards, so
   `sd_card.c` must compile everywhere, so the pinout (six `_Static_assert`)
   cannot go under `#if BOARD_HAS_SD`. Only the startup probe (`main.c`) and
   the `sd` console command (`console.c`) depend on it.
