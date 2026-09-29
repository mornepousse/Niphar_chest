# Niphar_chest

The **chest** of the [Niphargus](https://github.com/mornepousse/Niphargus) split
keyboard: an ESP32-P4 embedded in the left half, behind a USB hub, that only
wakes up when wired. “Lots of things, one at a time”:

- **Multi-ISO USB drive**: the microSD card exposes selectable bootable images
  (MSC), the sysadmin's rescue kit.
- **OpenPGP card over CCID** and **CR-HMAC security key over HID** — protocols
  taken from [KeSp_firmware](https://gitlab.com/harrael/KeSp_firmware)
  (`docs/OPENPGP_CARD.md`, `docs/SECURITY_KEY.md`). Nothing FIDO/CTAP was
  inherited from KeSp, which does not implement it either — it is marked
  “Phase 3” upstream — but U2F over CTAP-HID has since been written here and
  validated on hardware (see the status below).
- Removable **storage**.

**The chest has no selector of its own.** Unlike the standalone key, its console
has no power over USB modes: the keyboard's left half drives it over an SPI
link, and the chest publishes back what it is doing — including **the name of
the account** an operation targets, so the keyboard's screen can show *which*
account the owner is approving rather than only *what kind* of operation is
pending. The wire protocol is specified in
[`docs/LINK_CONTRACT.md`](docs/LINK_CONTRACT.md), which is the contract the
[KeSp_firmware](https://gitlab.com/harrael/KeSp_firmware) team implements
against.

Hardware: see [`docs/HARDWARE.md`](docs/HARDWARE.md) — contract verified against
the netlist (Niphargus review of 2026-08-06).

## Status

Three boards, a single firmware (see [`docs/HARDWARE.md`](docs/HARDWARE.md)):

- [x] **JC-ESP32P4-M3-DEV** (dev kit) — the first hardware that existed.
      No longer the daily board: `wt9932_key` is (see `.tripwire-variant`).
- [x] **wt9932_key** (WT9932P4-TINY module) — standalone OpenPGP security key,
      two buttons and one LED on the front panel, validated on hardware on
      2026-08-17 by a real physical press (see
      [`docs/HARDWARE.md`](docs/HARDWARE.md)).
- [x] **niphar_chest** (the chest) — **built and running**. Flashed on
      2026-09-05; runs the v3 link since 2026-09-29. Boots, installs the SPI
      slave, detects the microSD, publishes its register block.
- [x] **S3↔chest link, protocol v3** — 64 bytes of shared SPI registers plus a
      DMA channel. The chest publishes its state, the **label** of the armed
      operation, how many accounts it targets, the **active** USB mode and a
      *time-valid* bit; the keyboard requests a mode, confirms a press with an
      **instance echo**, and browses accounts over DMA. Contract and 24 test
      vectors in [`docs/LINK_CONTRACT.md`](docs/LINK_CONTRACT.md), regenerated
      by executing the code and pinned in `test/test_link_proto.c`.
      **Not yet exercised end to end**: the register block is published on real
      hardware, but no SPI master has read it and **no frame has crossed the DMA
      channel** — that needs the keyboard's bench.

- [x] **Wall-clock time** (`niphar-oath set-time`) — the chest has no RTC, so
      the host sets the time once per plug-in over the CCID channel the client
      already speaks (`INS 0x10`, outside the YKOATH set). The chest carries it
      forward on its monotonic clock, and **it erases itself on unplug** —
      the chest only exists while wired, so there is no stale time to
      invalidate. The *time-valid* bit never rises on a guessed time: without
      it the keyboard shows “NO TIME” rather than a code that would be wrong
      while looking right. Chain verified against the **official RFC 6238
      vectors** (`test/test_totp_rfc6238.c`), cross-checked with `oathtool` and
      `hashlib`. Not yet exercised on hardware.

- [x] **OATH/TOTP** (`usb mode oath`) — YKOATH applet over CCID, the sixth USB
      mode. Validated on hardware on 2026-08-19: the key returns the right TOTP
      code, cross-checked against `oathtool` at a controlled instant, and a
      physical press is required for every code. Host client in
      `tools/niphar-oath` — `ykman` needs a PC/SC daemon this machine
      deliberately does not run (see [`docs/HARDWARE.md`](docs/HARDWARE.md)).

- [x] **SSD1306 OLED screen** on `wt9932_key` — the key stops being blind: it
      announces what it is being asked to authorize before the press. Splash
      screen, four status screens in a double-height font, countdown bar,
      standby with a wandering logo. Verified by eye on the panel on
      2026-08-17; the confirmation screen and its bar are still to be seen,
      they need a real OpenPGP operation to arm (see
      [`docs/HARDWARE.md`](docs/HARDWARE.md)). **Retracted**: the “spontaneous
      mode switches” were involuntary presses by the owner, her hand brushing
      the button while handling the bare board — the MODE button works, and
      there is nothing to repair.
- [ ] Multi-ISO MSC
- [x] OpenPGP CCID + CR-HMAC OTP-HID integration (KeSp specs) — PGP validated
      end to end on hardware on 2026-08-07 (`gpg --card-status`, key
      generation, signature requiring physical confirmation — see
      [`docs/HARDWARE.md`](docs/HARDWARE.md)); OTP-HID checked only at USB
      enumeration, the real CR-HMAC exchange is still to be tested for want of
      HID tooling on the dev machine
- [ ] Flashing the embedded C6 via esp-hosted (the P4's radio, optional)
- [x] **U2F over CTAPHID** — INIT/PING/MSG wired up, `U2F_VERSION`,
      `U2F_REGISTER`/`AUTHENTICATE` validated on hardware (WT9932P4-TINY)
      including via `fido2-cred -M` (libfido2, 124 captures), crypto selftest
      at startup (`PASS`) and measured stack margin (3164/6144 B, 51.5 %, on the real signing path); the signing
      path was validated for real on 2026-08-18 — two real registrations, two
      distinct key handles and public keys, confirmed with `sec confirm` on the
      console; the earlier claim that the front-panel confirmation button was
      electrically open is retracted, it never was (see
      [`docs/HARDWARE.md`](docs/HARDWARE.md#fido2--u2f--2026-08-18)).
      `authenticatorGetInfo` (CTAP2) answers but is not advertised to CTAP2
      clients (`CTAPHID_CAPFLAG_CBOR` removed): `makeCredential`/
      `getAssertion` are still to be written, plan 2.
