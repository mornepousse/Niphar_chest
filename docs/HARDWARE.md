# Hardware contract — ESP32-P4 chest (Niphargus v2)

Source: sheet `p4.kicad_sch` of the Niphargus repository, verified pin by pin against
the netlist (review 2026-08-06). The module is a **JC-ESP32P4-M3 V0.2** (U16).

## Power — wired only

- AMS1117-3.3 (U17): +5V USB → `P4_3V3` rail. **The chest does not exist on battery**
  (design decision: 5 mA quiescent regulator accepted since it is powered by USB only).
- CHIP_PU: RC 10k → P4_3V3 + 1 µF → GND: boots automatically when USB arrives.
  **No reset button and no hardware access to download mode** — recovering from a
  firmware that breaks the USB-Serial-JTAG means soldering on the module (NEVER
  reassign GPIO24/25, never ENTER permanent deep sleep).

## USB — two distinct paths

| Path | Module pins | Role | CH334R hub |
|---|---|---|---|
| USB 2.0 OTG **HS** PHY | 39/40 (`ESP_USB_N/P`, chip USB_DM/DP 49/50) | Data/MSC/HID — the product | port 2 (permanent) |
| **USB-Serial-JTAG** | 42/43 (`USB1_P1_N/P` = GPIO24/25) | Flash/debug (esptool without strap) | port 3 via jumpers **JP1/JP2** (bridged by default, can be opened to isolate) |

The hub (CH334R, crystal-less mode) is powered from the USB 5 V: unplugged, this whole
subsystem is dead — that is intended.

## microSD — fixed SDMMC IOMUX

| Signal | GPIO | Pull-up |
|---|---|---|
| CLK | 43 | — (never a pull-up on CLK) |
| CMD | 44 | 10k |
| D0–D3 | 39, 40, 41, 42 | 10k each |

- Würth 693072010801 connector (hinged) — **no card-detect**: software detection by
  polling CMD.
- **Usage rule enacted: microSD cards inserted/removed POWERED OFF only**
  (no TVS on the SD lines — Mae's decision 2026-08-06).
- Speed: IO fixed at 3.3 V (no 1.8 V switching) → High-Speed 50 MHz max
  (~20-25 MB/s in practice), no SDR104. No series resistors (to keep in mind
  if there are integrity problems at bring-up).

### Powering the SD IOs — settled, nothing to wire

On the ESP32-P4, the microSD card's IOs are not powered from the microSD card's
rail but from a dedicated SoC rail:

> “ESP32-P4 SDMMC Host requires the IO voltage to be supplied externally via
> the **VDDPST_5 (SD_VREF)** pin. If the design doesn't require the higher speed
> SD modes, this pin can be simply connected to the 3.3V supply.”
> — ESP-IDF Programming Guide, *SDMMC Host Driver* (ESP32-P4),
> § Configuring Voltage Level.

Careful with the name: the datasheet calls this rail **`VDD_IO_5`** (pin 85 of the
package), and it is indeed the one that powers GPIO39-48 — table 2-1 “Pin Overview”,
*ESP32-P4 Series Datasheet v0.7* p. 16. Searching for “VDDPST” in a symbol
turns up nothing.

**This rail is internal to the JC-ESP32P4-M3 module**: it does not appear on the
chest's schematic and there is nothing to connect to it. The microSD connector's VDD,
for its part, comes from the board's regulator — two distinct things.

Verified empirically on 2026-08-06 on the dev kit, which carries the same
module: the microSD card is detected via the “external supply” path, without the
internal LDO channel 4. That is consistent with `LDO_VO4 not wired` and with IOs
fixed at 3.3 V without SDR104. The firmware polls both paths at startup anyway and
logs the one that was used (`sd info`) — if a different module were ever fitted,
the discrepancy would show at first boot.

## S3↔chest link — SPI, the chest as slave

> The protocol contract published to the `KeSp_firmware` team — register map,
> line parameters, presence detection, shared test vectors — lives in
> [`docs/LINK_CONTRACT.md`](LINK_CONTRACT.md). This section is the hardware
> record; that one is what the master side implements against.

Decided on 2026-08-06. The PCB was still editable; the pinout below is
the one retained on the chest side.

| signal | chest (P4) | S3 (keyboard) — **intent from 2026-08-06, OBSOLETE** |
|---|---|---|
| CS | GPIO7 | **GPIO3** |
| MOSI | GPIO8 | MOSI of SPI2, already routed (GPIO40) |
| SCK | GPIO9 | SCK of SPI2, already routed (GPIO38) |
| MISO | GPIO10 | MISO of SPI2, already routed (GPIO39) |
| IRQ (chest→S3) | GPIO11 | **GPIO46** |

> **This table is the routing, verified against the exported netlist on
> 2026-09-29.** `kicad-cli sch export netlist` on `Niphargus/hardware/pcb/`:
> net `CS_P4` joins U6 pin 15 (`GPIO3/TOUCH3/ADC1_CH2_15`, the S3) to U16
> pin 11 (`GPIO7_11`, the P4); net `IRQ_P4` joins U6 pin 16 (`GPIO46_16`) to
> U16 pin 15 (`GPIO11_15`).
>
> **A retracted correction, and the lesson it carries.** On 2026-09-05 this
> table was marked stale and “corrected” to IO7 / IO11 on the S3 side. That was
> wrong: IO7 and IO11 are the **chest-side** numbers, reported by mistake onto
> the S3 column. The suspicion was raised at the time — “those two numbers are
> exactly the ones `board.h` already declares on the P4 side” — and set aside
> anyway. The KeSp session caught it by exporting the netlist, which is the only
> source that settles a pinout. A number given from memory, even by the person
> who drew the board, is not a routing.
>
> **Two pull resistors, not in any earlier version of this document**:
> R48 (10 kΩ) pulls `CS_P4` up to **`P4_3V3` — the chest's rail**, and R49
> (10 kΩ) pulls `IRQ_P4` down to GND. R48 has a consequence for the keyboard:
> driving GPIO3 while the chest is unpowered pushes ~0.33 mA into a dead rail,
> the same reasoning that made the IRQ active-high. It also keeps the chest
> deselected whenever the S3 leaves the line alone.

This is the **native IOMUX quartet of SPI2** on the P4 (`spi_slave.rst:157-162`, values
for `esp32p4`), hence a direct path without the GPIO matrix. That matters: the driver
switches the whole bus over to the matrix as soon as a single signal is off-IOMUX, and
the matrix lengthens MISO's input delay — on a cable harness between two boards, that
margin is not to spare.

On the S3 side, the chest is a third client of the SPI2 bus already shared between the
NRF24 and the e-ink display (`KeSp_firmware`,
`boards/kase_half_left/board.h:52-58,78-79`), hence a cost of a single pin: the CS.

- **Mode 0 mandatory** (CPOL=0, CPHA=0), not out of preference: it is what
  keeps SCK low at idle, the condition that already makes the S3-side strapping
  `GPIO_NUM_45` safe (`VDD_SPI`: high at reset would put the keyboard's flash rail
  at 1.8 V).
- **IRQ active high, pull-down on the S3 side.** The chest is unpowered
  most of the time; a pull-up would inject current into a powered-off rail through
  the P4's protection diodes. GPIO46 is indeed an S3 strap, but it only
  controls the printing of ROM messages and its level is “Ignored” with
  the default eFuse (ESP32-S3 TRM v1.8, table 8.3-1, p. 536).
- **GPIO35 forbidden** — it is the only strap that decides between application boot and
  download mode (TRM table 11.2-2). GPIO34/36/37/38 remain free and without effect
  on boot as long as GPIO35 is high.
- **Interrupt line present** (GPIO11 → GPIO46), added after the fact. The
  chest can therefore demand the keyboard's attention, which will make an on-screen
  prompt possible rather than a mere confirmation relay.

On the dev kit, GPIO7-11 are taken (codec I2C on 7/8, I2S on 9/10,
amplifier on 11): the link cannot be wired there. This is the first real pinout
divergence between the two boards, hence the `boards/` split.

## The host can force download mode — settled

The contract says “no **hardware** access to download mode”, which is accurate —
and hides a software path which is, for its part, open to the host:

> “if the download mode flag is set when ESP32-P4 is reset, ESP32-P4 will
> reboot into download mode”
> — ESP32-P4 TRM, chap. 53, table 53.3-2, p. 2715: on the USB-Serial-JTAG's
> CDC-ACM, `RTS=0 / DTR=1` sets the flag, `RTS=1 / DTR=0` resets.

Confirmed in chap. 11 (p. 798): “USB Serial/JTAG Controller can also force
switch the chip to Joint Download Boot mode from SPI Boot mode”, disableable
only by `EFUSE_DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE`. Also verified
empirically on 2026-08-06: a reset sequence that pulled DTR low made the kit
start in `boot:0x16 DOWNLOAD` instead of launching the application.

**Today, of no consequence**: the host can already read and write everything through
the MSC, and any host software can reflash anyway.

**Tomorrow, it invalidates an assumption.** The model inherited from KeSp accepts
cleartext keys in NVS *because* extracting them would require physical access. Here,
host software enters download boot and dumps the entire flash, NVS included.

### Settled on 2026-08-07: the JP1/JP2 jumpers, removed in production

**Mae's decision.** The chest ships without its JP1/JP2 jumpers. The USB-Serial-JTAG
is then physically out of reach of the host, and so is the download mode trigger.

What makes this solution better than the two others considered (burning
`EFUSE_DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE` + Secure Boot, or accepting the risk):
**the jumper is both the security boundary and the recovery path.**
A software attacker has no path; you, with the board in hand, reposition the jumper
and reflash. Recovery becomes possible again, merely deliberate — where the eFuses
would have removed it forever.

Two useful clarifications.

**The dump itself goes through both ports.** Once the chip is in download mode,
the ROM serves the download over the USB-Serial-JTAG as well as over the high-speed
OTG — *ESP32-P4 Series Datasheet v0.7*, p. 37: “USB Download Boot:
USB-Serial-JTAG Download Boot, USB 2.0 OTG Download Boot”. What is closed
is the **triggering**: the RTS/DTR lines of the USB-Serial-JTAG's CDC-ACM
(TRM chap. 53, table 53.3-2, p. 2715). The firmware exposes no CDC on the
high-speed port — only MSC, CCID or HID — so there is nothing to manipulate there.

**The console disappears along with the jumpers.** `usb mode` and `sec confirm` will no
longer be reachable from the host on a production chest. That is consistent:
on the chest these two commands come from the S3 over the SPI link, not from the
console. The dev kit, for its part, keeps its jumpers and its console.

## What cannot be tested on the dev kit

The JC-ESP32P4-M3-DEV wires the microSD identically (verified pin by pin against the
manufacturer's BSP), so all the storage code can be validated on it. Three things
will never be put to the test there, because **the kit forgives and the chest does not**:

| | Dev kit | Chest |
|---|---|---|
| Recovery | CH340C + BOOTMODE button | nothing — soldering iron |
| C6 | `U0RXD`/`U0TXD`/`IO9` wired | NC |
| Power | 5 V USB **or** Li-ion (TLV62560) | USB only |

Direct consequence: the rules “never reassign GPIO24/25” and “never permanent
deep-sleep” cannot be verified at runtime. They hold by construction —
`_Static_assert` in `main/board_common.h` and greps in
`scripts/fast.sh` — and these guardrails must not be worked around “just
for a test”.

## OpenPGP CCID validation — 2026-08-07

Task 12 of the security port (`.superpowers/sdd/2026-08-07-portage-ccid-otp/`),
on the JC-ESP32P4-M3-DEV kit, host gpg 2.4.9 / scdaemon (internal CCID driver,
`pcscd` **inactive**, deliberately).

**Proven**:
- `usb mode pgp` then `gpg --card-status` sees the card: Application ID, serial
  derived from the MAC, key attributes (`nistp256 cv25519 nistp256`). The whole
  CCID → APDU → OpenPGP data objects path works.
- **Exclusivity**: `lsblk` shows no disk while gpg is talking
  to the card — a single set of USB descriptors at a time, as designed
  (`usb/usb_mode.c`).
- **Physical confirmation required**, tested in both directions on a signing
  key generated on the card (`GENERATE 0x47/80`, `PSO:CDS`
  `00 2A 9E 9A`, tested via raw `SCD PKSIGN` rather than the full `--card-edit`
  flow — see “deviations” below):
  - **without** `sec confirm`: the CCID worker sends a time-extension CCID
    frame (WTX) every ~1.5 s while the firmware
    waits for the press; after exactly 15 s (`SEC_CONFIRM_TIMEOUT_MS`), the
    card answers `SW=6985` (Conditions of use not satisfied) and scdaemon
    reports the failure.
  - **with** `sec confirm` typed during the waiting window: the card
    answers `SW=9000` with a 64-byte ECDSA P-256 signature, a few
    hundred ms after the simulated press. `Signature counter` goes to 1.
  - Both halves count: without the first, nothing proves that the
    confirmation is actually required; without the second, nothing proves
    that it is sufficient.

**Not proven**: the real CR-HMAC exchange of the OTP-HID mode (see task 11 —
verified at USB enumeration only, for lack of HID tooling on this machine).
Do not document OTP as validated while this test is missing.

### Three bugs found while validating, absent from the previous tasks

This task was not supposed to touch any code (see its brief); the three
fixes below turned up while trying to make
`gpg --card-status` work, not while hunting for bugs:

1. **`openpgp_do_init()`/`openpgp_card_load()` never called** (flagged by
   task 10) — the DO store started empty. Wired to entry into PGP mode
   (`usb/mode_pgp.c:mode_pgp_data_load()`, called by `usb_mode.c` after
   `usb_device_install()`), not at startup: see `CLAUDE.md` for the rationale.
2. **`nvs_flash_init()` never called anywhere** in the firmware — every
   NVS write (DO, PIN, keys) failed with `ESP_ERR_NVS_NOT_INITIALIZED`.
   Silent as long as nothing really read/wrote the NVS (before
   ccid.c made `openpgp_card_apdu` reachable, cf. task 10). Added in
   `main.c`, before USB.
3. **CCID descriptor not USB 2.0 compliant at high speed**:
   `KASE_CCID_ITF_DESC` froze `wMaxPacketSize` at 64 bytes for both
   bulk endpoints, at both speeds — copied as-is from KeSp (an
   ESP32-S3 dongle, never put to the test at that speed for CCID). The chest negotiates
   high speed (480 Mbps), where USB 2.0 mandates 512 as the ONLY
   legal value for a bulk endpoint (table 5-5 of the spec). The non-compliant
   descriptor let enumeration pass but corrupted the bulk exchanges
   on the host side (`scdaemon --debug-ccid-driver`: “unexpected bulk-in msg type
   (00)”, then timeout). Fixed by parameterizing the size (64 in FS, 512 in
   HS), on the model already in place for `TUD_MSC_DESCRIPTOR` in
   `mode_storage.c`.

   A fourth symptom tied to the same DMA/cache compliance defect: the
   static buffers of `ccid.c` (`s_out_buf`/`s_in_buf`/`s_wtx_buf`)
   were not aligned on the cache line (64 B), which is required on ESP32-P4
   for `esp_cache_msync()` to keep the CPU and the USB DMA coherent. Log
   observed: `cache: esp_cache_msync(112): start address ... not aligned`,
   at the exact addresses of those buffers (verified via `nm` on the ELF). Fixed
   with `CFG_TUSB_MEM_ALIGN` (already defined in `tusb_config.h`, used
   nowhere else in the ported code).

   Both — 64 B endpoint in HS and unaligned buffers — manifested
   as the same observable symptom on the host side (corrupted CCID responses);
   fixing one without the other would not have been enough. **KeSp (ESP32-S3) never
   has this problem**: the S3 does not have the same DMA cache-coherence architecture
   as the P4, hence the absence of any upstream precedent on these two points.

## Miscellaneous

- **Module's on-board C6: blank and not wired** (U0RXD/U0TXD/IO9 NC) — flashable
  only via the P4 (esp-hosted / OTA). IO9 floating = normal boot, safe.
- P4 straps (GPIO34-38): all NC; GPIO35 has an internal pull-up → SPI boot by default.
- 9 GND pins of the module wired; DSI/CSI/LDO_VO4 not wired (accepted).
- ~~Direct S3↔P4 link: NONE (abandoned) — the P4 talks to the host over USB,
  period.~~ **Retracted 2026-09-28.** This bullet was never dated and contradicts
  both the “S3↔chest link — SPI, the chest as slave” section and the 2026-09-05
  finding that the `CS_P4` and `IRQ_P4` nets exist in the KiCad project, wired on
  both sides. The link is implemented (`main/link/link_spi.c`) and the chest was
  flashed with it. Found while translating this document, not by a review.

## WT9932P4-TINY security-key board

Third board of the project — a standalone security key, with no microSD and no
link to a keyboard: two buttons and an addressable LED on the front panel take
their place. Pinout read from the manufacturer's schematic (`WT9932P4-TINY_1v2`, JLCEDA,
revised 2025-08-07).

**⚠️ Pinout not verified against hardware.** The schematic was read from an image
rendering by the manufacturer (1920 px), not from the PCB nor from an exported netlist —
unlike the chest's table at the top of this document, which is verified
pin by pin. The IO32/IO33 buttons are not yet soldered on Mae's
unit. **To be cross-checked against the module's silkscreen before any wiring** —
only the USB-Serial-JTAG port has actually been exercised (task 6, see below):
`chip_id` and then a full flash both went through successfully. The HS OTG (J4) has
carried no traffic — the firmware stayed in `USB_MODE_NONE` the whole
time — and the boot strap has not been tested in practice (no press on the
BOOT button; the download mode used for flashing goes through software
RTS/DTR on the USB-Serial-JTAG, not through IO35).

| Item | Pin(s) | Detail |
|---|---|---|
| WS2812 addressable LED | IO51 (DIN) | Powered at 5 V, data at 3.3 V — below the strict VIH threshold (0.7×VDD = 3.5 V). See hardware warning below. |
| Power indicator LED | R13 (1 kΩ) | Not controllable, hard-wired. |
| User buttons | IO32 (MODE), IO33 (CONFIRM) | To ground, internal pull-up, no external component. Not yet soldered — to be wired by Mae. |
| BOOT button | IO35 | P4 boot strap — **not exposed to the firmware**, see below. |
| RESET | CHIP_PU | Power-on RC, no dedicated reset button (like the chest). |
| J4 | OTG HS | USB 2.0 high-speed PHY — the product port (MSC/CCID/HID). |
| J3 | USB-Serial-JTAG | Only flash/debug path (GPIO24/25) — same rules as the chest. |
| microSD | — | No connector in the schematic: `BOARD_HAS_SD 0`. |

### Why GPIO35 (BOOT button) stays out of the firmware

GPIO35 is one of the five boot straps of the ESP32-P4: “ESP32-P4 has five
strapping pins: GPIO34, GPIO35, GPIO36, GPIO37, GPIO38” — *ESP32-P4 TRM*,
chap. 11.2, p. 795. Its level at reset alone decides between application boot and
download mode: table 11.2-2, p. 796 — `GPIO35 = 1` (default, internal pull-up)
selects the SPI Boot mode, `GPIO35 = 0` selects the Joint Download Boot,
independently of GPIO36/37/38.

The silicon would nevertheless allow it to be reused afterwards: “After the
reset is released, the strapping pins work as normal-function pins” (same
page, §11.2.1 / 11.2.2). The security-key board forgoes it all the same — three reasons:

1. A press during power-up would force download mode instead of
   starting the application: the key would vanish from the bus instead of announcing itself.
2. An accidental reset with the button held (CHIP_PU goes low then high again) produces
   the same effect during use.
3. `scripts/fast.sh` (guardrail #1) already excludes board headers from its
   grep on `GPIO_NUM_(24|25|35)` — a use of GPIO35 in `boards/wt9932_key/
   board.h` would therefore stay green while the meaning of the pin flipped there,
   from “reserved” to “user button”. The firmware must not
   depend on a guard that would not see it.

IO32 and IO33 were chosen instead because they are, together with IO26 to IO31,
the only P4 pins whose three columns in the GPIO table are empty —
no analog function, no LP GPIO, no restriction in the comment column (*ESP-IDF
Programming Guide*, “GPIO & RTC GPIO — ESP32-P4”, § GPIO Summary). They
come out on J7 and do not encroach on any block occupied by the sister boards
(microSD on GPIO39-48, S3 link on GPIO7-11).

### This board does not withstand a flash dump

Unlike the production chest (JP1/JP2 jumpers removed, see
above), **nothing physically isolates the USB-Serial-JTAG from the host on the
security-key board**: the BOOT button is on the front panel, and triggering software
download mode through RTS/DTR remains open as well (TRM chap. 53, table 53.3-2, p.
2715, cf. “The host can force download mode” above). A host — or
whoever physically handles the key — can dump the entire flash.

This is significant here because the flash contains the OpenPGP private keys in
cleartext (PGP mode, `usb/mode_pgp.c`): a full dump exposes them. The countermeasure
retained for the chest (removing the jumpers in production) does not apply to
this board — it has no equivalent jumpers to remove, and its purpose
(a portable key, buttons + LED on the front panel) precisely assumes an accessible
enclosure. No mitigation is proposed here; it is a known security
gap, to be handled separately (Secure Boot / Flash Encryption, out of scope of the
`sdkconfig.defaults*` shared by guardrail #3 of `fast.sh`).

### Validated on hardware — 2026-08-16

Task 6 of `.superpowers/sdd/2026-08-16-carte-cle-wt9932/`, real WT9932P4-TINY
module (MAC `30:ed:a0:e0:bc:5f`), overwriting the `jc_devkit` firmware that had
previously been flashed on it.

- `chip_id` confirms the chip before any flash: ESP32-P4 rev v1.0, MAC
  `30:ed:a0:e0:bc:5f` — so the port `by-id …-30:ED:A0:E0:BC:5F-if00` does designate
  this module, not another board on the dev machine.
- Boot measured by RTS-only reset (DTR high, never a strap into download mode)
  and raw reading of the port, without a TTY: `main_task: Calling app_main()` at
  t=832 ms, all the logs of `app_main()` (board, microSD skipped, `sec_gate`)
  on the same tick, `niphar>` prompt reached. With a terminal that answers
  linenoise's ANSI probe (`ESC[5n`, as any interactive terminal
  would), `main_task: Returned from app_main()` drops to t=919 ms — a
  net gain over the previous ~11 s (absent-SD polling that timed out). See
  “measurement deviation” below.
- `lsusb`: no `303a:4021` (the composite MSC/CCID/HID VID:PID) — only
  `303a:1001` (USB-Serial-JTAG) appears, confirming `USB_MODE_NONE` at
  startup on this board too.

**Measurement deviation, for anyone reproducing with `boot_capture.py` as-is**: this
script never answers the ANSI status request (`ESC[5n`) that
`esp_console_setup_prompt()` sends to detect a capable terminal
(`linenoiseProbe()`, `components/console/linenoise/linenoise.c`, 500 ms
budget). Without an answer, the probe exhausts its budget and the prompt falls back to
“dumb” mode — measured as a constant, reproducible delta of 1000 ms between
`Calling app_main()` and `Returned from app_main()` over three runs (832→1832,
819→1819, 832→1832). It is an artifact of the non-interactive capture script,
not a firmware regression: as soon as a responder exists on the host side (tested
by sending back `ESC[0n` upon receiving the probe), the same boot drops to 100 ms
(819→919). Both numbers are below the 11 s bar of the SD polling; the
second is the measurement faithful to what a user would see on a real
terminal.

### sec_confirm validation with a real button — 2026-08-17

Task 8 of `.superpowers/sdd/2026-08-16-carte-cle-wt9932/`, on the WT9932P4-TINY
security-key board, buttons IO32 (MODE)/IO33 (CONFIRM) freshly wired by Mae.
First time this project puts its physical-presence chain to the test: until then
`sec_confirm` had only a console command, of no security value since it was
triggerable by software, and the keyboard link that will carry the real integrated
confirmation does not exist.

**Proven, by a real press on the button — not by the console**:

- MODE press (IO32) → blue LED, `303a:4021` enumerates at **high speed**,
  `gpg --card-status` answers.
- **Key generation on the card**: `gpg` performs **three** signatures
  (self-signature of the identity, then one binding signature per subkey),
  each subject to `UIF Sign=on`.
- **Without a press**: expiry at 15 s (`SEC_CONFIRM_TIMEOUT_MS`), the card
  returns the status word **`6985`** (“Conditions of use not satisfied”),
  `gpg` gives up. **Observed twice.**
- **With a press** during the alternation: the operation goes through. **Observed
  twice**, then three times in a row to carry a full generation to completion.
- **One confirmation per operation, not per session** — established by the direct
  evidence above, not by the signature counter (see below for
  why that counter does not read as “n presses ⇒ +n”): without a press,
  `6985` observed twice; with a press during the alternation, the operation is
  granted. Nothing indicates a confirmation that would hold for a whole
  session — each press covers only the operation for which it was requested.
- Full generation completed: `pub nistp256
  3DEF9F107CB7FE6F02D5351E2F49F54486F3560C [SC]`, subkeys `[A]` nistp256 and
  `[E]` cv25519, identity “mae (coucou)”.
- PIN counters: a wrong admin PIN decrements the counter (3 → 2), and
  a successful verification **rearms** it (→ 3). Behavior compliant with the
  OpenPGP specification.
- **Full cycle to OTP mode and back**, real MODE press: in PGP mode,
  the kernel logs a **real disconnect** of the CCID device
  (`usb 3-3: USB disconnect, device number 76`) **before** the arrival of the
  next one (`new high-speed USB device number 79`), then
  `bInterfaceClass 3 Human Interface Device` and
  `hid-generic … hidraw9: USB HID v1.11 Keyboard [Mae PUGIN Coffre Niphar]`.
  Another MODE press → back to `bInterfaceClass 11 Chip/SmartCard`,
  `wMaxPacketSize 0x0200` (512 B) on both endpoints. It is not
  the appearance of the HID that counts, it is the prior disconnect: the card
  is not a composite device that would expose both its functions
  permanently, it really leaves the bus before the keyboard arrives.
  This is the founding principle of the project — “plenty of things, one at a time,
  never two at once” — observed on the wire and not inferred from the code: a
  host cannot talk to the OpenPGP card while the OTP key is
  exposed, it no longer exists.
- **CONFIRM press outside an armed operation: no flash**, verified by eye.
  Deliberate: flashing would signal that something happened when
  the press authorized nothing.
- **Real message signature**, not just key certificates:
  `echo "test alternance" | gpg --sign --armor` → alternation observed, CONFIRM
  press, and the signature verifies:
  ```
  gpg: Signature made lun. 17 août 2026 14:10:37 CEST
  gpg:       using ECDSA key 3DEF9F107CB7FE6F02D5351E2F49F54486F3560C
  gpg: Good signature from "mae (coucou) <mae.protonmail.com>" [ultimate]
  ```
  The card's signature counter went from 4 to 5, confirming that
  the operation really went through the card. This is the ordinary use case, the
  everyday one — the previous points only validated signatures internal to key
  generation.

The eight-point validation protocol (`.superpowers/sdd/2026-08-16-carte-cle-wt9932/tache-8-brief.md`)
is now **complete: eight points out of eight**, all obtained by a real press
on the button and none by the console command.

**Two usage findings, recorded here rather than in the spec**:

1. `gpg` nowhere warns that a key generation will request **three**
   confirmations, and displays nothing between them. Someone who is not watching
   the LED will believe it failed without understanding why.
2. The initial waiting pulse (brightness fade to 20/255 on the
   mode color) proved **too discreet in real use**. Replaced
   the same day by an **alternation between the mode color and red at full
   brightness**, judged “much more visible” by Mae. Accepted reservation: red
   then carries two meanings — “I am waiting” and “refused” — distinguished by
   duration (15 s of alternation versus a 120 ms flash); documented in the
   spec.

**The signature counter does not advance by one per press, and that is intended**:
during the full key generation above (three operations subject to
the UIF), the card's signature counter went from 2 to 4, i.e. +2 and
not +3. This is not an anomaly — the code explains the two mechanisms
at play in `main/security/openpgp_card.c`:

- `INTERNAL AUTHENTICATE` (INS=0x88, lines 1143-1163) is indeed guarded by
  the UIF, but deliberately does not advance the DS counter: the
  comment there cites OpenPGP 3.4 §7.2.10/§7.2.13, counter progression
  being a semantics specific to `PSO:CDS`, not to `INTERNAL AUTHENTICATE`;
- `GENERATE KEY` and `IMPORT` on the signature slot (lines 1030 and 1223)
  call `ds_counter_reset()` — the counter restarts from zero in mid-session,
  it does not increment.

So “n presses ⇒ +n on the counter” is false by design, not by anomaly:
depending on the instruction that requested the UIF, a granted press can leave
the counter unchanged (INTERNAL AUTHENTICATE) or even reset it to zero
(GENERATE KEY/IMPORT on the signature slot). The property “one confirmation
per operation” remains true; it simply cannot be read off that counter.

**Not proven**:

- The **S3 link** — absent from all three boards, the keyboard-integrated variant
  remains untested.
- The **CR-HMAC exchange** of the OTP mode — no HID tooling on this machine;
  the mode is verified only up to the kernel binding, never a
  challenge/response.
- **Resistance to a flash dump** — the BOOT button is on the front panel, nothing
  isolates the USB-Serial-JTAG, and the private keys live in the flash. The
  countermeasure planned for the production chest (JP1/JP2 jumpers removed in
  manufacturing) does not apply to this board, see “This board does not withstand
  a flash dump” above.

## FIDO2 / U2F — 2026-08-18

Closure of plan `.superpowers/sdd/2026-08-17-fido2-plan-1/` (eight tasks,
branch `fido2-u2f`). Scope: a working **U2F** authenticator on the
three boards; `authenticatorGetInfo` (CTAP2) answers but no CTAP2 credential
command exists — the CBOR decoder of `makeCredential`/`getAssertion`
is deferred to plan 2.

### Measured on hardware

Everything that follows comes from the WT9932P4-TINY security-key board (`sec source` →
“bouton en façade”, `main/security/sec_gate.c:31,37`) — not from the dev kit,
not from a console.

- `U2F_VERSION` → `"U2F_V2"` + `SW=0x9000`.
- `U2F_REGISTER` without confirmation → `SW=0x6985` (“conditions of use not
  satisfied”), after the presence timeout expires.
- `AUTHENTICATE` on an unknown *key handle* → `SW=0x6A80` — rejected by
  `fido_key_check()` before any crypto branching.
- Unknown INS → `SW=0x6D00`.
- **Capture of 124 consecutive `U2F_REGISTER` under `fido2-cred -M`
  (libfido2)**: one INIT then 124 requests over ~15 s at ~117 ms intervals,
  all answered `0x6985`, never a command refusal — the proof that
  `fido2-cred` takes the right path end to end and that only the button
  is missing. Side effect: 124 repetitions with no state drift, a free load
  test on the REGISTER path.
- **Crypto self-test at mode startup** (`u2f_selftest()`,
  `main/security/u2f.c:414`): credential derivation → public key,
  signature, signature/public-key verification, DER encoding, attestation
  signature, attestation-key ↔ embedded-certificate verification — the whole
  signing path, without ever going through the button. Log at
  FIDO mode startup: `selftest: PASS (credential + attestation)`.
- **`usb_task` stack margin at mode startup (self-test, GET_DESCRIPTOR):
  3548 bytes free out of 6144, MEASURED** (`uxTaskGetStackHighWaterMark()`,
  `main/usb/mode_fido.c`, `fido_selftest_once()`) — not estimated by analogy.
  The stack had been raised from 4096 to 6144 “by analogy with `ccid_worker`”
  (task 7, before measurement); it was the self-test that made a first measurement
  possible by actually exercising the signing path.
  **Corrected at the final branch review (I2, below): this path is
  NOT the worst case** — see the measurement on the real path just after.

### Real hardware validation of the signing path — 2026-08-18 (final branch review)

**A confirmation was possible from the start: `sec confirm` (console),
not the button.** `wt9932_key` defines `BOARD_CONSOLE_ACTIONS 1`
(`boards/wt9932_key/board.h:25`), so `sec_gate_console_confirm()` compiles
and grants the slot armed by U2F — independently of the state of the front-panel
MODE/CONFIRM button. The branch review had asserted several times
that the electrically open button prevented any validation of the signing
path; **that was wrong twice over**. The reported fact (“no real signature
produced”) was accurate; the cause it invoked was not, neither in its
consequence — the console crutch remained available and simply was not
used — nor in its premise: the button was never open (see
“Retracted — there was never a button fault”, below). One and the same
error was thus built on an invented fact and then reasoned from
for two days.

**Protocol**: `usb mode fido` in the console, `fido2-cred -M` (via `nix-shell`,
`shell.nix`) in a second session to launch a real `U2F_REGISTER`
(RP id `niphar-test.example`), then `sec confirm` typed in the console
session during the 15 s window.

**Result — two real registrations, two distinct keys (direct proof
that C1 is fixed)**:

| | key handle (32 bytes, nonce‖tag) | COSE public key Y (32 bytes) |
|---|---|---|
| registration 1 | `98852ce4e63598462d72ab55ddcb25961eb6e7fb420dc72900cc3109144f5a5b` | `7e6e060d768de5712e2761bcac115bcf72b04c4cf4bdaf8c0733a54cfdabf3dd` |
| registration 2 | `96dbd5cd017c39a4973597075cb24f88434c8afb7169cd8482346cb448437dd0` | `8923a06ca4629af341cba5e57509d6df4e01fe01e4c4b3c24afe403d71e7d4fc` |

Each key handle is exactly 32 bytes (verified after base64 decoding);
the two columns are entirely different between the two rows, not
only on a prefix.

Both `fido2-cred -M` succeeded (`fmt: fido-u2f`, exit code 0,
embedded `Niphargus FIDO Attestation` attestation certificate, DER signature
present) — before C1, these two registrations on the same domain
would have produced the SAME key handle and the SAME public key (nonce
systematically null); here they differ entirely, on the 32 bytes
of the key handle as well as on the public key. This is the end-to-end proof,
above the unit test `test_zero_nonce_derivation_is_predictable_not_random`
of `test/test_fido_key.c`, which can only bound the pure logic of
`fido_key.c` — not the wiring bug of `u2f.c` itself (see that test
file for why).

**I2 — stack margin measured on the REAL path, from `handle_message()`
(`main/usb/mode_fido.c`, case `CTAPHID_CMD_MSG`), after the `U2F_REGISTER`
confirmed above (real derivation + ECDSA signature + DER encoding, not
the fixed message of the selftest):**

**3164 bytes free out of 6144 — 51.5% margin.** Neither the 57% put forward by
the self-test (measured on the wrong path, GET_DESCRIPTOR rather than
SET_REPORT), nor the ~33% that the final branch review feared by
extrapolation (“~1520 bytes of frames further down”): the real gap between the
two measurement points is 384 bytes (3548 → 3164), not 1520. This figure
replaces all the previous ones as the margin reference for `usb_task` in
FIDO mode; the mention “conservative measurement” is withdrawn, it described
a measurement that was not one.

### Decisions whose consequence is visible

**`CTAPHID_CAPFLAG_CBOR` is removed from the last byte of INIT**
(`main/usb/mode_fido.c:158-190`), so `fido2-token -I` no longer describes the
key (`caps: 0x00`). **This is not a failure.**

`libfido2` decides CTAP1 vs CTAP2 on this transport bit, never on the
content of `versions` (`fido_dev_is_fido2()`, `src/dev.c:515`): once the
bit is set and `authenticatorGetInfo` answers, the library commits to
CTAP2 for the whole session and never tries `u2f_register()` again
(`src/cred.c:217-232`) — even after a `makeCredential` failure. This is
exactly what was observed on hardware before this removal:
`fido2-cred -M` failed with `FIDO_ERR_INVALID_COMMAND` instead of taking the
U2F path. `authenticatorGetInfo` remains implemented and correct in `ctap2.c`, it
simply still always answers over CTAPHID, just never advertised as available to
CTAP2 clients.

It will come back when `authenticatorMakeCredential` **and**
`authenticatorGetAssertion` both exist (plan 2) — never
before, never to make `fido2-token -I` alone reappear.

### AAGUID — a model identifier, never a unit identifier

`76365535-e558-4f54-b32d-5fc79426a628` (`main/security/ctap2.c:24`) is fixed
and **identical across all units** of this board. It must never
vary from one board to another: a per-unit AAGUID would be an
identifier correlatable across different relying party sites — exactly what
WebAuthn seeks to avoid by distinguishing AAGUID (model, public) from
*credential ID* (unit × site, private). Regenerating it per board
would turn a privacy standard into a tracking tool.

### The master key lives in NVS in cleartext, not in eFuse

An accepted deviation from the plan, documented at the top of `main/security/fido_master.c`,
not an oversight. The specification called for K_master in a read-protected
eFuse block, behind the P4's hardware HMAC peripheral; burning
an eFuse is **irreversible**, and the owner did not authorize it.

Concrete consequence: a dump of the NVS partition (see “The host can
force download mode” and “This board does not withstand a flash
dump” above/below in this document) reveals the master key in cleartext,
and therefore **all the identifiers** (*credential ID*) derived from it — not
only that of one session. A read-protected eFuse would have made
this extraction impossible even with full physical access; here,
it costs no more than a flash dump. Moving to the eFuse remains a separate
piece of work, to be requested explicitly.

### SSD1306 OLED screen — 2026-08-17

Added on the security-key board only. Neither the kit nor the chest carries one: the
`#if defined(BOARD_OLED_SCL)` guard of `main/hmi/screen.c` reduces the file to
4 bytes of `.text` on the other two boards, verified with `nm`.

| | |
|---|---|
| controller | **SSD1306**, not an SH1106 — measured, see below |
| panel | 128×64, fully drivable |
| bus | I²C, `SCL = IO53`, `SDA = IO54`, address **0x3C**, 400 kHz |
| pull-ups | **two external 4.7 kΩ**, fitted by hand by the owner |
| init sequence | `AE 20 00 A1 C8 8D 14 A6 AF` |

**The P4's internal pull-ups must stay disabled** in the driver
(`enable_internal_pullup = false`): the external ones exist, and enabling them
would describe a setup that is not this one.

#### What the probe found, before a single line of driver was written

A throwaway probe — added, flashed, then deleted — found **two hardware
faults**:

1. **A badly soldered pin.** Symptom: spurious addresses, **not
   reproducible** from one scan to the next (first session `0x14/0x3A/0x74`;
   second session ten others, none in common). A real device always answers
   at the same address — this was noise on a floating line. It is
   the non-reproducibility that made the diagnosis, not the addresses
   themselves.
2. **No external pull-up.** Added afterwards.

It also corrected three assumptions out of four: only the address `0x3C` was
right. The controller, the presence of the pull-ups and the health of the panel were
assumed without proof.

#### Electrical residue — the margin is thin

**Spurious addresses persist even after the pull-ups were fitted**, still
different at each scan. The SSD1306 answers and accepts its commands, so
the screen works — but this bus has no margin.

Probable causes, in order: the length of the flying wires, or the absence of a
100 nF decoupling capacitor as close as possible to the module.

**If intermittent errors appear later, look there BEFORE
suspecting the driver.** That is the reason this subsection exists: the
symptom is already measured, it would be absurd to re-diagnose it.

The driver accounts for it: after twenty frames lost in a row (one second),
`screen_task()` replays the initialization sequence, because retransmitting
the same frame only repairs a transient loss — not a controller that has lost its
internal state.

#### Validated by eye, on the panel

- **Double-height** font by pixel doubling: legible, confirmed by the
  owner. The “real” 12×16 glyph table is therefore not necessary.
- **Half-scale logo** (32×32) on the splash screen and in standby: legible.
  The full-size logo overlapped the splash text — fixed.
- **Wandering standby**: the logo roams, validated.

#### Measured

- **`screen_task` stack margin: 2004 bytes free out of 3072**, read off the
  board ten seconds after startup, once all the deep paths had been
  taken. The task therefore uses ~1068 bytes, of which about 800 for the
  I²C driver and the logs — a share that a disassembly of `screen.c.obj` alone
  could not see (it gave ~256 bytes).

#### Not proven

- **The confirmation screen and its countdown bar.** They require a real
  OpenPGP operation to arm; neither has yet been seen on the panel.
- **The behavior on a lasting I²C error.** The reinitialization after twenty
  failures is written but never triggered in real conditions.

#### Retracted — there was never a button fault

**This section described a hardware fault that does not exist.** It asserted,
from 2026-08-17 to 2026-08-18, that the MODE button (IO32) was electrically
open and called for rework with a soldering iron. That is false, and the way
it became false deserves to be kept.

**The measurement that settles it, 2026-08-18.** Console listening, the owner presses
MODE four times, deliberately:

```
I (2630508) usb_mode: mode USB : clé FIDO2   -> carte OpenPGP
I (2632408) usb_mode: mode USB : carte OpenPGP -> clé CR-HMAC
I (2634228) usb_mode: mode USB : clé CR-HMAC -> clé FIDO2
I (2635918) usb_mode: mode USB : clé FIDO2   -> carte OpenPGP
```

Four presses, four switches, gaps of 1.9 / 1.8 / 1.7 s. No bounce,
no supernumerary switch: the 20 ms debounce does its job and
the contact is clean. **The MODE button works.**

**What the “spontaneous switches” really were.** Involuntary presses
by the owner — her hand brushing the button while handling the bare
board. Every observation fits there, with nothing left over.

**And this is where the original reasoning turned against itself.**
I had made “two to three switches in under two seconds, then minutes of
silence” the signature of a twitching contact, ruling out I²C
coupling because it would have produced a regular rhythm. The rhythm measured
above on **deliberate** presses is the same: 1.7 to 1.9 s. A tight
group of a few switches spaced on the order of a second is not the
signature of a wire that vibrates — **it is the signature of a hand.** I had
explained those gaps by the duration of a full `usb_mode_set()`; that was
a rationalization built after the fact to save the hypothesis.

**Why the 2026-08-17 probe saw nothing.** It measured `IO32=1 IO33=1`
at rest and zero edges, then I wrote that “a physical press on MODE produced
neither an edge nor a switch”. **No deliberate press had been tried on
IO32 during that window.** This is exactly the mistake already made on IO33 the
same day, and corrected the next day by a simple “yes I did press”:
concluding that a pin is dead from an absence of signal, without making sure that a
signal had been emitted. Two pins reading 1 at rest say nothing about their
health, and a probe that sees nothing proves nothing as long as nobody has
pressed.

**The frozen-logo symptom was real, its cause was not.** The screen stayed stuck
on the full logo because standby triggers after 60 s without a change
of state and because each switch resets that counter — the mechanism is exact, but
it was triggered by real presses, not by a faulty contact.

**Nothing to repair.** No soldering-iron work is needed on
this board.

**The lesson in method**, which cost two false diagnoses in two days on the
two buttons: a hardware hypothesis is not validated by piling up
observations that it explains well. It is validated by asking the person
who has the board in hand to produce the event, then watching whether the device
sees it. Here, thirty seconds of listening and four presses were enough — and nothing
of what was written in the meantime got any closer to the truth.

### OATH/TOTP validation — 2026-08-19

YKOATH applet on CCID, `wt9932_key` board (MAC `30:ED:A0:E0:BC:5F`), firmware
`d1c1e02`, client `tools/niphar-oath`.

**The code returned by the key matches `oathtool` at the controlled instant.**

```
T0=1787116452   compteur = T0/30 = 59570548

cle    : 563782   (valide encore 17 s)
oathtool --totp -b JBSWY3DPEHPK3PXP --now=@1787116452  -> 563782
meme secret, fenetre SUIVANTE (@T0+30)                 -> 788748
```

A single measurement proves the whole chain: the time counter supplied by
the host is correctly interpreted, the secret was indeed persisted in NVS,
HMAC-SHA1 and the RFC 4226 truncation are correct.

~~And **the card does not apply the modulo** — if it did, the result could not
match.~~ **That inference is wrong, and it was mine — caught on 2026-09-28 while
translating this document.** `x % 10^6` is idempotent: a card that applied it and
a host that applied it again would agree just the same. The match therefore
proves the time counter, the stored secret and the truncation — it does **not**
discriminate between “the card applies the modulo” and “it does not”. What
settles that is reading the code: `oath_dynamic_binary()` returns the raw 31-bit
value, and `ykman`'s `_format_code` does the modulo host-side. A measurement that
cannot distinguish two hypotheses proves neither, however satisfying the numbers
look.

**A measurement trap, met and then dismissed.** A first cross-check gave
`660163` against `367131` — an apparent disagreement, caused by a rollover of the
thirty-second slot between the two commands. Comparing a TOTP code without
pinning the instant proves nothing: `oathtool --now=@<epoch>` is mandatory.

**What `pcscd` changes, and why it is not enabled.** `ykman` requires the
PC/SC daemon, absent from this machine — and Mae's NixOS configuration
deliberately disables it, because it would grab the CCID interface ahead of
`scdaemon`, on which her git signing and her card-based SSH authentication
depend. The `libccid` list counts 1274 identifiers, no Espressif among them: declaring
ourselves in it would be precisely what breaks that chain. Hence `niphar-oath`,
which speaks CCID over libusb as `scdaemon` already does. **The firmware remains
standard YKOATH**: on a machine where `pcscd` runs, `ykman` will work.

**Put to the test**: `add` (PUT, the first real write by `sec_store` since that
module has existed), `list` (LIST), `code` (CALCULATE with confirmation), the
fifteen-second wait with absorption of the time-extension frames, and the
`6985` refusal when the press does not come.

**The button does command the output of a code — 2026-08-19.** Mae ran
`niphar-oath code Test:mae` and pressed: the code came out in two seconds.
Two earlier attempts **without** a press had expired at fifteen seconds on
`6985`. The presence gate is therefore required, seen, and sufficient — on the
OATH path itself, and not only by extrapolation from U2F.

`add` of an unknown name and `list` require no press, as specified:
`GitHub:mae` was provisioned without a gesture, and the store holds two accounts.

**Not put to the test, to be done with Mae**: the screen during the wait — that it does name
the requested account (decision 4 of the spec) and that the countdown bar empties
there only once; `delete` and `reset` with their distinct screen (“EFFACER”, “RESET OATH” and the
number of accounts); and the migration of the twelve Proton accounts.

> This paragraph announced “TOUT EFFACER” until the final branch review.
> That was false: `screen_op_short(SEC_OP_OATH_RESET)` returns `"RESET OATH"`, and
> cannot return anything else — twelve characters would overflow the ten of the
> double-height font (`test_op_short_fits_the_double_height_font`). A doc
> that announces a label the screen will never display leads to concluding there is a
> non-existent fault at the moment of verification by eye.

### S3↔chest link — the pinout exists in the PCB, not in the keyboard's documentation

*Established on 2026-09-05, by reading the KiCad project rather than the derived documentation.*

`docs/NIPHARGUS_V2_HARDWARE.md` of `KeSp_firmware` — though “verified against the
netlist” — **mentions neither CS nor IRQ for the P4**. It goes so far as to list,
among the “unaddressed design consequences”, the fact that three
slaves share the bus “each with its own CS”, as if the P4's remained to be
invented.

The nets do exist, and the source of truth is the PCB project. But our own
reading of it was wrong until 2026-09-29 — see the correction below. The nets
**`CS_P4` and `IRQ_P4` exist** in `Niphargus/hardware/pcb/` — present in
`s3.kicad_sch` as well as in `p4.kicad_sch`, hence wired on both sides.

| net | S3 side (U6) | chest side (U16) | pull |
|---|---|---|---|
| `CS_P4` | **GPIO3** (pin 15) | GPIO7 (pin 11, `BOARD_LINK_CS`) | R48 10 kΩ → `P4_3V3` |
| `IRQ_P4` | **GPIO46** (pin 16) | GPIO11 (pin 15, `BOARD_LINK_IRQ`) | R49 10 kΩ → GND |
| SCK / MISO / MOSI | 38 / 39 / 40, **shared** | 9 / 10 / 8 | — |

**Corrected on 2026-09-29 from the exported netlist.** The first version of this
section put IO7 and IO11 on the S3 side, from a number given by Mae in
conversation. Those are the **chest-side** numbers. `kicad-cli sch export
netlist` settles it: `CS_P4` = U6 pin 15 (`GPIO3/TOUCH3/ADC1_CH2_15`) ↔ U16
pin 11 (`GPIO7_11`); `IRQ_P4` = U6 pin 16 (`GPIO46_16`) ↔ U16 pin 15
(`GPIO11_15`). The KeSp session caught it and exported the netlist — the only
source that settles a pinout.

**R48 pulls `CS_P4` up to the chest's own rail**, `P4_3V3`. Two consequences for
the keyboard: driving GPIO3 while the chest is unpowered pushes ~0.33 mA into a
dead rail (the same reasoning that made the IRQ active-high), and R48 keeps the
chest deselected whenever the S3 leaves the line alone.

**The bus is shared three ways** — nRF24 (`CSN` GPIO16), Sharp display (`LCD_CS`
GPIO14, active HIGH), and the chest. Two consequences that bear on our
firmware:

- **The chest must release MISO when not selected.** A slave that keeps MISO
  driven holds the bus even while working perfectly — and makes the radio
  go silent. That is written on the keyboard side after an hour of diagnosis.
- **Nothing arbitrates access** between the three slaves as of today.

**Debt to report back to KeSp**: this pinout is missing from their hardware document, and
its absence has already cost an hour of diagnosis on 2026-09-05 — an unprogrammed
P4 nails SCK/MISO/MOSI, the nRF24 goes silent, and the cause is found
last.
