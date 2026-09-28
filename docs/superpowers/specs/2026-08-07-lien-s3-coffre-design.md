# S3↔chest link — SPI, with the chest as slave

Design validated on 2026-08-07. Second increment of the chest firmware.

## 1. Problem

The chest has **no source of physical presence** whatsoever. No button, no
contact, no connection to the keyboard: the only channel that talks to it is
USB, which is precisely the adversary in the threat model.

Yet the strength of `KeSp_firmware`'s OpenPGP stack rests on a single lock:
`sec_confirm` — a real press on a key, which host malware cannot fabricate. The
PIN, on the other hand, can be captured. Without the link, OpenPGP on the chest
would be protected by the PIN alone, strictly weaker than the existing dongle,
and a compliant FIDO would be impossible: CTAP requires a test of user
presence.

On top of that there is a functional need: being able to **drive the chest's
options from the keyboard** — selecting a bootable image, enabling functions —
without going through the host.

The link brings both at once.

## 2. Scope

In scope: the SPI transport, the protocol and its versioning, absence
detection, and a single message — “the user has confirmed”.

Out of scope, each with its own spec: driving the options, the OpenPGP CCID
port, FIDO/CTAP, and the S3 side in `KeSp_firmware`.

## 3. Pinout

| signal | chest (P4) | keyboard (S3) |
|---|---|---|
| CS | GPIO7 | GPIO3 |
| MOSI | GPIO8 | existing SPI2 MOSI |
| SCK | GPIO9 | existing SPI2 SCK |
| MISO | GPIO10 | existing SPI2 MISO |
| IRQ (chest→S3) | GPIO11 | GPIO46 |

On the chest side, this is SPI2's native IOMUX quartet (`spi_slave.rst:157-162`,
`esp32p4` values), hence a direct path. This is not cosmetic: the driver
switches **the whole** bus over to the GPIO matrix as soon as a single signal
is off-IOMUX, and the matrix lengthens MISO's input delay — on a harness
between two boards, that margin matters.

On the keyboard side, the chest is a third client of the SPI2 bus already
shared between the NRF24 and the e-ink (`KeSp_firmware`,
`boards/kase_half_left/board.h:52-58,78-79`), hence a cost of a single pin: the
CS.

### Three non-negotiable constraints

1. **Mode 0** (CPOL=0, CPHA=0). Not a preference: it is what keeps SCK low at
   idle, the condition that already makes the `GPIO_NUM_45` strap safe on the
   S3 side in the existing code.
2. **P4 GPIO35 forbidden.** It is the only strap that chooses between
   application boot and download mode (TRM table 11.2-2). Since the chest has
   no emergency button, getting it wrong costs a soldering iron.
3. **IRQ active high, pull-down on the S3 side.** The chest only lives while
   wired, whereas the left half runs on battery: it is **unpowered most of the
   time**, and its GPIO11 is then high-impedance. A pull-up would inject
   current into a dead rail through the P4's protection diodes. With a
   pull-down, chest absent = line at rest, nothing flows.

### The IRQ lands on an S3 strapping pin, without consequence

`GPIO0, GPIO3, GPIO45 and GPIO46` are the ESP32-S3's strapping pins
(`esp-idf/docs/en/api-reference/peripherals/gpio/esp32s3.inc:252`), and the IRQ
lands on GPIO46. That deserves examination, not worry:

- **GPIO46** only controls whether ROM messages are printed on UART0. With the
  `EFUSE_UART_PRINT_CONTROL` eFuse at its default value, its level at reset is
  explicitly marked “Ignored” — ESP32-S3 TRM v1.8, table 8.3-1, p. 536.
- **GPIO45**, on the other hand, chooses the flash rail voltage (low → 3.3 V,
  high → 1.8 V). Routing an active-high IRQ there could have prevented the
  keyboard from booting. That pin is in any case already `BOARD_NRF_SPI_SCK` on
  `kase_half_left`.

**Firmware invariant kept nonetheless:** the chest never asserts IO11 until the
S3 has spoken to it at least once. It costs nothing, it keeps the line quiet
during the keyboard's boot, and it protects against a future change to that
eFuse. It is, however, no longer a security rule in the sense that the GPIO35
prohibition is: degrading it would no longer break anything.

## 4. Architecture

```
main/link/
├── link_proto.{c,h}   pure logic: register map, frames, CRC
├── link_spi.{c,h}     transport: spi_slave_hd, IRQ line
└── sec_confirm.{c,h}  ported from KeSp_firmware, unchanged
```

**`link_proto`** contains no ESP-IDF call and compiles on the host. That is
where everything that can be wrong without being visible lives: register map
serialisation, absence detection, frame framing and CRC, version comparison. It
is also, and this is no accident, **the project's first pure logic** — hence
the first piece subject to the TDD norm in `CLAUDE.md`.

**`link_spi`** does transport only: `spi_slave_hd` on `board.h`'s pinout,
publishing the registers, driving the IRQ line. It decides nothing.

**`sec_confirm`** is taken as-is from `KeSp_firmware/main/security/` — a pure
module, without NVS or hardware, already covered by host tests that we port
along with it. Rewriting it would be a security regression for nothing.

### Two tiers, deliberately

**Shared registers** — readable and writable by the master *without the chest
firmware having prepared anything*, since it is the hardware that answers. That
is what makes diagnosis possible even with the chest hung, and it is the reason
for choosing `spi_slave_hd` over the classic slave: the latter requires the
slave to have posted a buffer before each transaction, which would demand a
“ready” line we do not have.

**Twenty bytes, five 32-bit words, and no word shared between the two ends.**
That is the structuring constraint of this table, and it takes precedence over
compactness.

| offset | word | field | owner |
|---|---|---|---|
| 0x00-0x03 | 0 | magic word `NIPH` | chest→S3, presence |
| 0x04 | 1 | protocol version | chest→S3 |
| 0x05 | 1 | state (bits: SD card present, USB mounted, ready) | chest→S3 |
| 0x06-0x07 | 1 | code of the operation awaiting confirmation (little-endian) | chest→S3 |
| 0x08-0x0B | 2 | counter of consumed confirmations (little-endian) | chest→S3 |
| 0x0C-0x0D | 3 | CRC16 over 0x00..0x0B (little-endian) | chest→S3 |
| 0x0E-0x0F | 3 | reserved, zeroed | chest→S3 |
| 0x10 | 4 | user confirmation | S3→chest |
| 0x11-0x13 | 4 | reserved, zeroed | S3→chest |

The chest owns words 0 to 3 (`0x00..0x0F`) and **publishes them in a single
block**; the master owns word 4 (`0x10..0x13`).

**Why word alignment, and not just separation of fields.** The `spi_slave_hd`
shared buffer is written in 32-bit words on the application side, and in bytes
on the master side (`spi_slave_hd.rst`, “Writing/Reading Shared Registers”). A
write that does not fill a whole word therefore goes through a
read-modify-write in the driver. As long as one of the chest's fields sits in
the master's word, republishing that field — the CRC, typically, which changes
on every state change — forces that read-back, and **a press by the owner
arriving during those few cycles is lost**. The window is narrow and not silent
(the confirmation counter does not move, so the master can retry), but it is a
lost gesture on a link whose only role is to carry gestures.

The first version of this table put the CRC at `0x0E` and the master's byte at
`0x0C`: the same word. Separating the two ends into distinct words closes the
window **at the source** instead of narrowing it. Fixed while the master did
not yet exist — that was the only moment when the change was free; later it
would have required reflashing both repos together.

The CRC covers only `0x00..0x0B`, that is, only the chest's fields, and **not**
the master's range: including it would invalidate the block on every legitimate
write from the S3. The reserved bytes are zeroed, each written by its owner — a
field added later in word 4 will not force the chest to change.

A single write from the chest crosses the master's word, and the protocol
requires it: reclaiming the confirmation byte after reading it, without which
the same press would be replayed. It only happens immediately after a press
already received, at the moment when the master is precisely waiting to see the
counter move.

**Semantics of `LINK_STATE_READY`** — frozen here because the master will use
it to decide whether it can ask for anything: the bit means **`app_main()` ran
to completion**. All the chest's initialisations have been attempted (microSD,
USB, confirmation source, HMI, screen) and the console is running. It does
**not** say they succeeded: they are not fatal one by one, and the chest
deliberately starts up without exposing anything (`USB_MODE_NONE`). The actual
state of the subsystems is read from the other bits. Its value is inverted: as
long as it is zero on a chest that is present, startup is in progress and a
request would land on half-installed modules.

**Data channel** for longer messages — specified here, **not implemented in
this increment**. It will carry the driving of the options.

### Frame format (data channel)

```
[0]    SOF 0xA5
[1]    version
[2]    opcode
[3]    payload length (0-247)
[4..]  payload
[n-2]  CRC16, little-endian
```

SPI provides neither framing nor error detection: both are the protocol's
responsibility. The version is in **every** frame, not only in the registers:
the two repos will not always be flashed together, and an unexpected version
must cause the frame to be rejected rather than misinterpreted.

## 5. Absence is the normal case

The chest only exists while plugged into USB; the left half lives mostly on
battery. **Chest absent is not an error**: the S3 must neither wait, nor log,
nor spend power on it.

A floating MISO reads as `0x00` or `0xFF` depending on termination. The magic
word distinguishes those two cases from a real chest, and that is exactly the
kind of detail one believes obvious and codes backwards — hence dedicated
tests.

## 6. Error handling

- Unknown protocol version: frame rejected, counter incremented, no side
  effect. Never a partial interpretation.
- Bad CRC or inconsistent length: silent rejection. SPI has no acknowledgement;
  the master will retry if it cares.
- Confirmation received with no operation armed: no effect, like
  `sec_confirm_authorize()` outside the `PENDING` state. It is not an error, it
  is an out-of-context press.
- Link init failure: logged, the chest carries on. The link is a supplement;
  MSC and the console must survive its absence.

## 7. The security invariant

Just one, and it lives in `KeSp_firmware`: **the S3 only writes a confirmation
on a real key press.** Never from the CDC protocol, which talks to the host —
otherwise we reopen to the host the one lock it was never supposed to be able
to cross, and the whole point of the link disappears.

That invariant is outside this repo but is part of this design. It deserves a
`check.sh` rule on the KeSp side when the counterpart is written there.

## 8. Verification

On the host, with no hardware:

- frame framing, inconsistent lengths, bad CRC, truncated frame;
- rejection of an unknown version;
- absence detection on `0x00` and on `0xFF`;
- round-trip of the register map;
- **word ownership**: every byte of the block has exactly one owner, and the
  four bytes of a given word all have the same one — expressed over the
  ownership ranges, not over a copied-out list of offsets, otherwise the test
  would only prove the copy;
- `pack_status()` writes **no** byte of the master's range;
- `sec_confirm` behaviour (tests ported from KeSp).

On the dev kit, what remains verifiable: the `jc_devkit` build passes with the
link disabled, and **nothing that worked regresses** — the microSD, MSC, and
the throughputs measured on the foundation (9.4 and 5.3 MiB/s).

**What cannot be validated, and that is new.** On the kit, GPIO7-11 are taken
by the audio codec and its I2C. This link is therefore the first piece of the
project written without being able to be proven on the bench — everything else
was, right away. Left as debt until the revised board: real enumeration, IRQ
latency, hot plug and unplug, and above all the `VDD_SPI` strapping window —
the one point where a software error would prevent the **keyboard** from
booting.

## 9. Decisions and rejected alternatives

| Decision | Rejected alternative | Reason |
|---|---|---|
| SPI, chest as slave | I2C on the S3's existing bus | I2C would have cost no pin on the S3 side, but a chest that blocks the bus (clock stretching, crash) would also freeze the keyboard's screen |
| `spi_slave_hd` | classic `spi_slave` | the classic slave requires a buffer posted before each transaction; without a “ready” line, a chest busy serving MSC would lose transactions |
| Confirmation pushed by the user | the S3 polls in a loop and shows the prompt | permanent polling costs battery on a half that runs off it, while the chest is absent most of the time. The prompt on the e-ink will become possible again via the IRQ line, in a later increment |
| `sec_confirm` ported unmodified | rewrite adapted to the chest | pure module, already validated and tested on hardware; rewriting it would be a gratuitous security regression |
