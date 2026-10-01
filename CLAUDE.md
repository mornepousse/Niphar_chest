# Niphar_chest — Claude Code instructions

ESP32-P4 firmware for the **chest** of the
[Niphargus](https://github.com/mornepousse/Niphargus) split keyboard: a P4
embedded in the left half, behind a USB hub, that only wakes up when wired.
Built with ESP-IDF 5.5.

## Repo

- **Origin**: https://github.com/mornepousse/Niphar_chest
- **Local**: `~/Documents/GitHub/Niphar_chest/`
- **Neighbours**: [KeSp_firmware](https://gitlab.com/harrael/KeSp_firmware) —
  OpenPGP CCID stack already validated on hardware (`main/security/`), to be
  reused for the PGP/FIDO side; specs in its `docs/OPENPGP_CARD.md` and
  `docs/SECURITY_KEY.md`.

## Irreversible hardware constraints

Full contract: [`docs/HARDWARE.md`](docs/HARDWARE.md), verified against the
netlist. Two rules override everything else:

1. **Never reassign GPIO24/25.** That is the USB-Serial-JTAG, and it is the
   chest's only flashing and debugging path: no reset button, no hardware way
   into download mode. A firmware that breaks that link is repaired with a
   soldering iron.
2. **Never enter permanent deep sleep**, for the same reason.

`main/board_common.h` carries `_Static_assert`s, and `scripts/fast.sh` greps,
that fail the build on these two points. Do not work around them.

**The dev kit forgives, the chest does not.** The JC-ESP32P4-M3-DEV has a CH340C
and a BOOTMODE button; these rules will therefore never be checked at runtime on
it. They hold by construction, not by experience.

## The chest exposes nothing at startup

From cold, the chest boots in `USB_MODE_NONE` (`main/usb/usb_mode.h`): **no**
working USB interface is installed — no drive, no smart card, no HID. That is
deliberate (“lots of things, one at a time”, never two at once) and **it is the
normal behaviour**, not a failure: a chest that has just been flashed or reset
will show up in no `lsusb`/`lsblk`/`gpg --card-status` as long as nothing has
been asked of it.

The selector is never USB itself. **Which selector depends on the board**, and
the difference is not cosmetic:

- `jc_devkit` / `wt9932_key` — the serial console (`main/console/console.c`).
- `niphar_chest` — **the link**, byte `0x12` written by the S3
  (`main/link/link_spi.c`). The chest's own console has no power over modes:
  `boards/niphar_chest/board.h` sets `BOARD_CONSOLE_ACTIONS 0`. A chest with no
  keyboard attached therefore stays in `USB_MODE_NONE` and there is nothing on
  it that can change that — which is the behaviour, not a fault. The chest
  publishes the mode it actually holds at `0x0D` so the keyboard shows the real
  state rather than its own request.

The console commands, where the console has power:

```
usb mode none       # nothing exposed — the idle state
usb mode storage     # microSD card as MSC
usb mode pgp          # OpenPGP card over CCID
usb mode otp          # CR-HMAC key over HID
usb mode fido          # U2F/CTAP-HID authenticator
usb mode oath          # YKOATH TOTP accounts over CCID
```

Each switch first uninstalls the current mode (`usb_device_uninstall()` — a real
USB disconnect as seen by the host) before installing the next: at any instant,
at most one set of descriptors is present. See `main/usb/usb_mode.c`.

**Side effect worth knowing**: `usb mode pgp` reloads the persistent OpenPGP
state (DOs, PINs, keys — `usb/mode_pgp.c:mode_pgp_data_load()`) on **every**
entry into the mode, not only at first boot — necessary because
`ccid_drv_init()` rearms the factory PINs in RAM on every switch. Loading that
state at startup (like `sd_probe()`/`sec_gate_init()`) would have been simpler
but contradicts this principle: putting private keys in RAM before anyone has
asked for PGP mode makes no sense. Detail and proof on hardware:
[`docs/HARDWARE.md`](docs/HARDWARE.md#openpgp-ccid-validation--2026-08-07).

## Build

Three boards. `jc_devkit` and `niphar_chest` diverge only on the S3↔chest link,
absent from the kit; everything else (microSD card, USB) is common and lives in
`main/board_common.h`. `wt9932_key` is the third — the standalone security key
(WT9932P4-TINY), with no S3 link and no microSD card, with buttons and an LED on
the front panel — see [`docs/HARDWARE.md`](docs/HARDWARE.md) for its pinout.

| board | S3 link | hardware |
|---|---|---|
| `jc_devkit` | no | the kit, the first hardware that existed |
| `niphar_chest` | yes | the chest, built and flashed on 2026-09-05 |
| `wt9932_key` *(current variant, `.tripwire-variant`)* | no | the standalone key, the only hardware actually flashed day to day |

```bash
source ~/esp/esp-idf/export.sh
idf.py -B build_wt9932_key -DBOARD=wt9932_key -DSDKCONFIG=build_wt9932_key/sdkconfig build
```

`.tripwire-variant` (committed, read by `.esp-dev.yml`) carries the name of the
board that `/esp-build`/`/esp-flash`/`/esp-cycle` build and flash by default —
**check its value before flashing**: a stale default has already caused the
`jc_devkit` firmware (no screen) to be flashed onto the `wt9932_key` board, an incident
documented in `docs/HARDWARE.md`. The direction of the mistake remains
asymmetric between boards that have an S3 link: flashing `jc_devkit` onto a
chest merely deprives it of the link, whereas the opposite would send SPI into
the I2C bus of the kit's audio codec — `wt9932_key` has neither bus, so it is
only concerned by the first general rule (never GPIO24/25, never permanent deep
sleep).

No source file includes a board path: `${BOARD_DIR}` is first in the include
order, so `#include "board.h"` resolves to the selected board.

Flash and monitor: `/esp-build`, `/esp-flash`, `/esp-cycle`, `/esp-monitor`
(config in `.esp-dev.yml`).

**Serial port — careful.** Never widen the glob to `/dev/ttyACM*`: on this
machine, `/dev/ttyACM0` is the KaSe V2 Debug keyboard (`cafe:4001`). The P4's
port is designated by
`/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*-if00`.

`idf.py monitor` requires a real TTY and therefore does not work from an agent.
To capture a boot without a TTY: reset by pulsing RTS alone (DTR low would strap
the chip into download mode) then read the port.

## Versioning

Source of truth: the git tag `vX.Y.Z`, read at build time by
`git describe --tags`. No VERSION file. The `NIPHAR_VERSION` macro is applied to
the `main` component only: passing it as a global `add_compile_definitions()`
would recompile the whole project on every commit.

## Release

Source of truth: the git tag `vX.Y.Z`, read at build time by `git describe
--tags`. No VERSION file, and **no manifest duplicates the version** — nothing
to keep in sync.

Build the artefacts (all three boards, as `scripts/full.sh` does):

```bash
idf.py -B build_$v -DBOARD=$v -DSDKCONFIG=build_$v/sdkconfig build
```

Attach, renamed per board: `build_*/niphar_chest.bin` →
`niphar_chest-vX.Y.Z-<board>.bin`.

### Smoke test

`./scripts/check.sh` compiles and runs the host tests. **It has never seen the
hardware work** — the DMA alignment was found at the bench, `op_count` by
re-reading, and the wire by watching a counter. These five items are what the
check cannot reach. Each one caught something real at least once.

1. **The chest boots.** `link` installed, microSD detected, **no `E (`/`W (`** in
   the boot log. (The DMA alignment fault showed up here, as one warning line.)
2. **The keyboard selects a mode and the chest exposes it** — `lsusb` shows
   `303a:4021`, and the `link` state byte gains the *mounted* bit.
3. **Console `link`** — the block decodes with `link_proto_parse_status()`, the
   master's access counter **grows between two readings in one boot** (opening
   the port resets the chip, so separate invocations cannot show growth).
4. **A TOTP code matches `oathtool`** at the same instant, with the neighbouring
   windows differing. This is the only item that reaches `cr_hmac_sha1`, which
   goes through mbedtls and is therefore unreachable from the host tests.
5. **A cancel clears the prompt** without waiting out the fifteen seconds.

Items 4 and 5 need the owner: a physical press, and a keyboard that implements
the cancel.

## Anti-regression workflow (MANDATORY)

Single source of truth: `scripts/check.sh`.
- `./scripts/check.sh --fast` — the chest's hardware guardrails + incremental
  ESP-IDF build (~seconds). `--force` reruns despite skip-if-already-green.
- `./scripts/check.sh` — fast + the full rebuild of the three boards (pre-push, CI).
- Red → read `.git/tripwire/last-fail.log`, fix. Never bypassed without a written reason.

`check.sh` is a templated file that tripwire updates rewrite: a guardrail or a
test suite is added in `scripts/fast.sh` or `scripts/full.sh`, never here.

Claude Code hooks: `PostToolUse` → `--fast` as a notice; `Stop` → `--fast`, blocks;
`pre-push` → full, blocks. Off: `TRIPWIRE_OFF=1` (session) or `.tripwire-off`
(repo, untracked); `git push --no-verify` for the pre-push.
Enabling the git hooks, once per clone: `./scripts/install-hooks.sh`.
New pure logic: red test first, green after.
Read before touching a source, if present: `.tripwire-divergences` (accepted
departures from the scaffold; red if a declared pattern disappears).
The why of each mechanism: the tripwire plugin README, not here.

### When to invoke the project's agents

`.claude/agents/` contains five agents specialized to the chest:

| Agent | When |
|---|---|
| `niphar-test-author` | writing or restructuring tests; standing up the host harness when the first pure logic arrives |
| `niphar-code-reviewer` | before a merge to `main` or a release, and after any non-trivial code |
| `niphar-debugger` | broken build, red test, panic, drive missing on the host side, silent SD card |
| `niphar-maintainer` | dependency bump, ESP-IDF upgrade, partition or sdkconfig change |
| `niphar-security-auditor` | adding a handler for external input (MSC, descriptors, SD parsing, future CCID/FIDO), and before a release |

### TDD norm — new pure logic
Every new pure-logic function (LBA addressing computation, transfer splitting,
header parsing, state machines): test written **first**, added to the fast
phase's test suite. The test must be red before the implementation, green after,
and parallel-safe (no mutated global state).

The host harness exists: `test/`, compiled by CMake with the machine's compiler,
run by `scripts/fast.sh` **before** the firmware build. The ratchet is active
(`.tripwire-testcount`, committed) and pre-push refuses a decrease.

A test is only worth something if it bites: after writing it, introduce a
transient bug that should make it fail, check that it goes red, revert. That is
what distinguishes a test from a decorative assertion.

Only pure logic goes into `test/` — no ESP-IDF call, otherwise it does not
compile on the host. That constraint is a design tool: what is not testable is
almost always what mixes computation and hardware.

### Model economy (subagents)
The check.sh pipeline makes it possible to drop down a tier WITHOUT risk of
hallucination, but only where an oracle catches the mistake:
- **Economy model (haiku) OK**: transcribing code that is already specified,
  mechanical refactors, cited extraction (`file:line` mandatory) — the check,
  the compilation or the cross-checking of the citations catch the drift.
- **Never below sonnet**: review, audit, debug, **and writing test assertions**
  — a tautological assertion or a hallucinated verdict pass the mechanical
  oracle green. Judgment does not drop a tier.
- Every economy task MUST end on a green `./scripts/check.sh --fast`, and a
  rewired/written test MUST prove that it bites (transient bug → red → revert).
