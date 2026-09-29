# S3↔chest link — protocol contract

**Audience**: the `KeSp_firmware` team, writing the master side.
**Authority**: this document. The chest is the SPI *slave* and publishes the
register map, so the protocol is defined here and consumed there.
**Written on**: 2026-09-29, from the implementation, not from the design spec.

The chest side is implemented and builds; it has **never been exercised against
a real master**, because on the dev kit GPIO7-11 are taken by the audio codec.
So: the bytes below are what the chest's code actually produces and accepts —
that part is verifiable today, and section 10 gives you the vectors to check it
against. The *electrical* side has not been proven, and section 2 says exactly
where the uncertainty is.

## 0. How to read this

Three kinds of statement, marked throughout:

- **Established** — enforced by the code, by a `_Static_assert`, or by a host
  test. Changing it breaks our build. You can rely on it.
- **Chosen for lack of a specification** — the chest had to pick something to
  compile, and did, with the master not yet existing. These are *defaults*, not
  requirements. If they are wrong for you, say so; the cost of changing them now
  is one commit on our side.
- **Yours** — a decision that belongs to KeSp. Section 9 collects them.

Source files, should you want to read them rather than trust this page:

| what | file |
|---|---|
| register map, CRC, absence, version | [`main/link/link_proto.h`](../main/link/link_proto.h), [`main/link/link_proto.c`](../main/link/link_proto.c) |
| CRC function | [`main/sys/cr_crc16.c`](../main/sys/cr_crc16.c) |
| transport, pins, IRQ | [`main/link/link_spi.c`](../main/link/link_spi.c) |
| chest-side pinout | [`boards/niphar_chest/board.h`](../boards/niphar_chest/board.h) |
| hardware record, incidents | [`docs/HARDWARE.md`](HARDWARE.md) |

There is also a design spec,
[`docs/superpowers/specs/2026-08-07-lien-s3-coffre-design.md`](superpowers/specs/2026-08-07-lien-s3-coffre-design.md).
**It has been overtaken twice.** Where it disagrees with the code, the code
wins; section 11 lists every disagreement we found while writing this.

## 1. The register map

Twenty bytes, five 32-bit words, in the `spi_slave_hd` **shared register**
buffer. They are readable and writable by the master *without the chest's
firmware having prepared anything* — the SPI peripheral answers on its own.
That is deliberate: it means you can still read the chest's state while its
firmware is hung, and it is why we chose `spi_slave_hd` over the classic slave
driver (which would require the slave to post a buffer before every
transaction, and hence a "ready" line we do not have).

| offset | word | size | field | owner | notes |
|---|---|---|---|---|---|
| `0x00`–`0x03` | 0 | 4 | magic word `NIPH` | chest→S3 | `4E 49 50 48`, in that byte order |
| `0x04` | 1 | 1 | protocol version | chest→S3 | currently `0x01` |
| `0x05` | 1 | 1 | state bits | chest→S3 | see below |
| `0x06`–`0x07` | 1 | 2 | pending operation | chest→S3 | little-endian, `0` = none armed |
| `0x08`–`0x0B` | 2 | 4 | consumed-confirmation counter | chest→S3 | little-endian, free-running |
| `0x0C`–`0x0D` | 3 | 2 | CRC16 over `0x00`–`0x0B` | chest→S3 | little-endian |
| `0x0E`–`0x0F` | 3 | 2 | reserved, written `0x00` by the chest | chest→S3 | |
| `0x10` | 4 | 1 | user confirmation | S3→chest | see section 5 |
| `0x11`–`0x13` | 4 | 3 | reserved | S3→chest | **yours**, the chest never writes them |

**Established.** The offsets are `LINK_REG_*` in `link_proto.h`. The two
ownership ranges are `LINK_REG_CHEST_BASE`/`_LEN` = `0x00`/`0x10` and
`LINK_REG_MASTER_BASE`/`_LEN` = `0x10`/`0x04`.

### State bits (offset `0x05`)

| bit | mask | name | meaning |
|---|---|---|---|
| 0 | `0x01` | `SD_PRESENT` | a microSD card is present |
| 1 | `0x02` | `USB_MOUNTED` | a USB descriptor set is installed (mode ≠ none) |
| 2 | `0x04` | `READY` | `app_main()` ran to completion |

`READY` deserves a precise reading, because you will use it to decide whether
you may ask the chest for anything. It means **all** of the chest's
initialisations have been *attempted* and the console is running. It does
**not** mean they succeeded: they are not fatal one by one, and the chest
deliberately starts up exposing no USB interface at all (that is its normal
idle state, not a failure). The actual state of the subsystems is read from the
other two bits.

Read it the other way round and it is useful: as long as `READY` is zero on a
chest that is *present*, startup is in progress, and a request would land on
half-installed modules.

Bits 3–7 are unassigned and currently read as zero. Treat unknown bits as
reserved rather than as an error; the version byte is what gates
interpretation.

### Pending operation (`0x06`–`0x07`)

The code of the operation currently waiting for a user confirmation, or `0` when
nothing is armed. The values are the chest's `sec_op_t` enum
(`main/security/sec_confirm.h`): `1` = OpenPGP signature (PSO:CDS), `2` =
decrypt, `3` = internal authenticate, `4` = CR-HMAC OTP, `5`/`6` = FIDO
register/authenticate, then the OATH operations. **That enum is not frozen by
this contract** — it is the chest's, and new operations get appended. If you
display a label for it, treat an unknown code as "the chest is asking for a
confirmation" rather than as a protocol error.

### Confirmation counter (`0x08`–`0x0B`)

Counts the confirmations the chest has **relayed** to its security layer — not
the ones that were *granted*. A press arriving with nothing armed still
increments it. That distinction is what lets you tell "my write arrived" from
"my write did something": watch the counter move to confirm delivery, watch the
pending-operation field clear to see the effect.

It is a free-running `uint32_t`, never reset except by a chest reboot. Compare
against your last reading; do not assume it starts at zero when you connect.

### The CRC, and why it stops where it does

**Established.** CRC16 over exactly `0x00`–`0x0B` (12 bytes,
`LINK_REG_CRC_SPAN`), stored little-endian at `0x0C`–`0x0D`.

Algorithm — reflected, polynomial `0x8408` (i.e. `0x1021` reflected), init
`0xFFFF`, **no final XOR**. Check value over the ASCII string `"123456789"` is
**`0x6F91`**. That is CRC-16/MCRF4XX in the catalogue's naming. It is
`cr_crc16()` in `main/sys/cr_crc16.c`, which our repo records as taken verbatim
from `KeSp_firmware/main/security/cr_crc16.c` — so you should already have the
identical function. Please check the check value against yours rather than
against the name: our own header comment calls it "CRC-16/X-25", which is wrong
(X-25 XORs `0xFFFF` at the end and checks as `0x906E`). The code, not the
comment, is what runs.

**Why the CRC stops at `0x0B` and does not cover the master's word.** If it
covered `0x10`–`0x13`, then every legitimate write of yours — putting `0x5A` in
the confirmation byte — would make the block fail its own CRC, on both sides,
for the entire time the byte sits there. The chest would be publishing a block
that reads as corrupt precisely when something is happening. So the CRC is a
chest→S3 integrity check on chest→S3 fields only. Your word is outside it, and
you may put whatever you like in `0x11`–`0x13` later without invalidating
anything.

There is no integrity check in the S3→chest direction. Section 5 explains what
stands in for one.

### Why five words, and no word shared

This is the one structural constraint of the layout, and it takes precedence
over compactness.

The `spi_slave_hd` shared buffer is written **in 32-bit words** on the
application side and **in bytes** on the master side. A write that does not fill
a whole word goes through a read-modify-write inside the driver. So as long as
one of the chest's fields lives in the master's word, republishing that field —
the CRC, typically, which changes on every state change — forces that read-back,
and a press of yours arriving during those few cycles is **lost**. The window is
narrow and not silent (the counter does not move, so you would retry), but it is
a lost gesture on a link whose only job is to carry gestures.

The first version of this map put the CRC at `0x0E` and the confirmation byte at
`0x0C` — the same word. Separating the two ends into distinct words closes the
window at the source instead of narrowing it. It was fixed while the master did
not yet exist, which was the only moment it was free.

Consequences you can rely on:

- the chest publishes `0x00`–`0x0F` as **one aligned four-word write**, never
  touching `0x10`–`0x13`;
- a confirmation you have written and the chest has not yet read **survives**
  every state republication;
- four `_Static_assert`s in `link_spi.c` fail the chest's build if this ever
  stops holding.

The one place the chest writes into your word is reclaiming the confirmation
byte after reading it (section 5) — it has to, or the same press would replay
forever.

## 2. Pinout

### Chest side (ESP32-P4) — established

From `boards/niphar_chest/board.h`. This is SPI2's **native IOMUX quartet** on
the P4, which is why it is these four pins and not others: the driver switches
the whole bus onto the GPIO matrix as soon as a single signal is off-IOMUX, and
the matrix lengthens MISO's input delay — on a harness between two boards that
margin is not to spare. The chest's init passes
`SPICOMMON_BUSFLAG_NATIVE_PINS`, so it fails loudly rather than degrading
silently.

| signal | P4 | direction |
|---|---|---|
| `CS` | GPIO7 | S3 → chest |
| `MOSI` | GPIO8 | S3 → chest |
| `SCK` | GPIO9 | S3 → chest |
| `MISO` | GPIO10 | chest → S3 |
| `IRQ` | GPIO11 | chest → S3 |

### S3 side — **to be re-verified before soldering**

| net | S3 | shared with |
|---|---|---|
| `CS_P4` | **IO7** | nobody (one pin per slave) |
| `IRQ_P4` | **IO11** | nobody |
| `SCK` | IO38 | nRF24, Sharp display |
| `MISO` | IO39 | nRF24, Sharp display |
| `MOSI` | IO40 | nRF24, Sharp display |

> **Read this before you conclude anything from a silent link.**
>
> `CS_P4` and `IRQ_P4` **do exist** as nets in the `Niphargus` KiCad project,
> in `s3.kicad_sch` as well as `p4.kicad_sch` — wired on both sides. We
> established that on 2026-09-05 by reading the PCB project, because
> `KeSp_firmware/docs/NIPHARGUS_V2_HARDWARE.md` mentions neither CS nor IRQ for
> the P4, and even lists "three slaves each with its own CS" among its
> *unaddressed* design consequences, as if the P4's remained to be invented.
> That document is out of date on this point; the PCB is the source of truth.
>
> **But the IO7 / IO11 assignment comes from Mae, by hand, not from an
> automated read of the schematic.** Pin names live in the symbol library, not
> in the sheet file, so following the wire by eye would not have been proof. It
> has not been traced in the schematic. Please re-verify it on your side before
> soldering, and before concluding from a silence that the firmware is at fault.
>
> An earlier table in our own `docs/HARDWARE.md` said GPIO3 for CS and GPIO46
> for IRQ. **That was a design intent from 2026-08-06, not a routing**, and it
> is obsolete: your own `NIPHARGUS_V2_HARDWARE.md` classes GPIO3 and GPIO46
> among the unwired pins. If you find GPIO3/GPIO46 anywhere — including in our
> design spec, which still carries them — it is the stale value. The chest
> column, by contrast, has never moved.

## 3. Line parameters

Frozen by `link_spi.c` because the code had to compile and the master did not
exist. **All of these are "chosen for lack of a specification" except mode 0.**

| parameter | value | status |
|---|---|---|
| SPI mode | **0** (CPOL=0, CPHA=0) | see below — *we believe this one is forced* |
| bit order | **MSB first**, command, address and data alike | chosen (driver default; `flags = 0`, i.e. neither `SPI_SLAVE_HD_TXBIT_LSBFIRST` nor `..._RXBIT_LSBFIRST`) |
| command phase | **8 bits** | chosen (Espressif's `spi_slave_hd` default) |
| address phase | **8 bits** | chosen |
| dummy phase | **8 bits** | chosen |
| data width | **1-bit** (single MOSI/MISO), no dual/quad | chosen; `quadwp`/`quadhd` left unwired |
| CS polarity | **active LOW** | chosen (driver default) — **read the warning below** |
| clock rate | set by you; the chest is a slave and imposes none | yours |
| host | SPI2 on the P4 | established |

### Mode 0

`link_spi.c` calls it "the only acceptable one here", and the reason is not a
preference: mode 0 is what keeps **SCK low at idle**. On a bus shared three
ways, a slave that demanded idle-high would impose its mode on the other two —
and on the S3 side, idle-low is already the condition that makes the `GPIO45`
strap safe (`VDD_SPI`: high at reset puts the keyboard's flash rail at 1.8 V).
We believe this is forced by your hardware rather than chosen by us, but the
conclusion is about *your* board, so please confirm it.

### CS polarity — the one that will bite silently

**The chest's CS is active LOW.** That is the ESP-IDF slave default, and we did
not select it deliberately; it came with `spics_io_num` and `flags = 0`.

It is the **opposite** of the Sharp display on the same bus, whose `LCD_CS`
(GPIO14) is active **HIGH**. If the master's chip-select handling for the chest
is written by analogy with the display's — which is the natural thing to do,
they are two lines on the same bus in the same file — the chest is **never
selected**, MISO stays released, and every read comes back as `0xFF` or `0x00`.
Which our own `link_proto_is_absent()` will dutifully report as *"chest
absent"* — the normal state. No error, no log, no symptom. Just a link that
never works and a chest that looks unplugged.

This is exactly the failure mode this document exists to prevent. If active-low
is inconvenient on your side, say so: it is one line in our slot config.

### Master-side transaction shape

Half-duplex, following Espressif's `spi_slave_hd` protocol. To read the
register block:

```
CMD 0x02 (RDBUF) | ADDR 0x00 | 8 dummy bits | 20 bytes out (slave→master)
```

To write the confirmation byte:

```
CMD 0x01 (WRBUF) | ADDR 0x10 | 8 dummy bits | 1 byte in (master→slave)
```

Command `0x02` is RDBUF, `0x01` is WRBUF; the address phase carries the byte
offset in the register block. ESP-IDF's own reference master helpers
(`essl_spi_rdbuf` / `essl_spi_wrbuf`) send the dummy phase on both directions,
so we have kept `dummy_bits = 8` symmetric. If your master's half-duplex
driver handles the write direction without a dummy phase, that is a real
mismatch and we should settle it before you solder.

No DMA data channel is implemented in this increment (`max_transfer_sz = 0`,
`queue_size = 1`). The shared registers are the whole protocol today. The design
spec describes a framed data channel (`SOF 0xA5`, opcode, length, CRC) for
driving the chest's options — **that does not exist in the code**, in either
`link_proto` or `link_spi`. Do not implement against it.

## 4. Presence detection, and absence as the normal case

**A chest that is absent is the ordinary state, not a fault.** The keyboard's
left half runs on battery; the chest only wakes when the assembly is wired to a
host. The master must neither wait for it, nor log it as an error, nor spend
power polling it hard.

A bus with no slave answering reads uniformly — `0x00` or `0xFF` depending on
termination. `link_proto_is_absent(regs, len)` returns true when **every** byte
of the block is `0x00`, or **every** byte is `0xFF`. Uniformity is required in
full: a single differing byte proves somebody is answering, and mistaking a
talkative chest for a dead line would be worse than the reverse.

The chest's own parse routine, `link_proto_parse_status(regs, len, out)`,
applies its checks in this order and returns `false` on any of them without
touching `out`:

1. `len < 20` → reject (a short transfer must not be read past its buffer);
2. the block is *absent* by the rule above → reject — **checked before the
   magic word**, because absence is the expected case, not an anomaly;
3. magic word ≠ `4E 49 50 48` → reject;
4. version byte ≠ the version we know → reject (section 8);
5. CRC over `0x00`–`0x0B` ≠ the stored CRC → reject;
6. otherwise accept, and fill `out` with version, state, pending op, counter.

We suggest the master distinguish these outcomes, because they mean very
different things:

- **absent** → nothing plugged in. Normal. Say nothing, slow down.
- **present but rejected** (magic, version or CRC) → something *is* answering
  and it is not the chest you expect, or the bus is degrading. Worth a log.
- **present and valid** → read the state bits.

Note that a chest which has just booted publishes a valid block with all state
bits clear *before* it is ready. That is deliberate: the chest publishes the
register block before it starts its link task, so that a master polling during
that window sees "present, not ready" rather than "absent". Vector V9 in
section 10 is exactly that block.

## 5. The confirmation byte

**Offset `0x10`. The only value the chest accepts is `0x5A`.**

The chest polls this byte every 20 ms. On any non-zero value it **immediately
writes `0x00` back**, before deciding anything, and then:

- `0x5A` → a real press. Relayed to the security layer; the confirmation
  counter at `0x08` increments.
- anything else non-zero → discarded, and logged as
  `octet de confirmation inattendu 0x..`. The counter does not move.

### Why `0x5A` and not `1`

Because noise on a bus produces `1`, and does not produce `0x5A`.

`0x00` and `0xFF` are what a floating line reads as, so neither can be the
signal. A single stuck or glitching bit on a line at rest yields `0x01`, `0x02`,
`0x80` — any of the powers of two. `0x5A` is `0b01011010`: an alternating
pattern, five bit positions away from `0x00` and three from `0xFF`, reachable
from neither by one accident. It is not a checksum and it is not security — the
security invariant lives on your side, not ours (section 9) — it is a cheap
filter that keeps an electrical fault from being read as a human gesture.

### The handshake, from the master's side

1. Read the block. See `pending_op ≠ 0` (and `READY` set).
2. Obtain a **real key press** from the owner.
3. Write `0x5A` to `0x10`.
4. Poll the block. The confirmation counter increments → the chest received it.
   The pending operation clears → the chest acted on it.

Step 3 is a single-byte write into the master's own word. The chest's reclaim in
step 4 is a single-byte write *into your word*, which the driver performs as a
read-modify-write — so a second press landing in those few cycles would be lost.
That window only opens immediately after a press already received, at the moment
you are waiting to see the counter move, and a second press there would be a
duplicate anyway. We could not close it without writing your three reserved
bytes, which would set a trap for the first field you put there.

A confirmation with nothing armed is **not an error**: the chest relays it, the
security layer ignores it, the counter still moves. One gesture, one
authorisation — the byte is never left in place to be replayed against the next
operation armed.

## 6. The interrupt line

| property | value |
|---|---|
| direction | chest → S3, `IRQ_P4`, P4 GPIO11 |
| polarity | **active HIGH** |
| idle | driven LOW by the chest while it is powered |
| termination | **pull-down on the S3 side** — required, see below |
| asserted when | a confirmation is pending **and** the master has spoken at least once |
| shared with | nobody; it is the chest's only output on this connector |

**Active high is the opposite of the usual convention**, and the opposite of the
only other interrupt signal on the keyboard (the nRF24, active LOW with a
pull-up). Writing this one "by symmetry with its neighbour" produces a link that
never wakes anything, with no error message anywhere.

The reason is electrical, not stylistic: the chest is **unpowered most of the
time**, and GPIO11 is then high-impedance. A pull-up would inject current into a
dead rail through the P4's protection diodes. With a pull-down, chest absent =
line at rest = nothing flows.

**The chest never asserts this line before the master has written to or read
from the shared registers at least once.** That invariant is enforced in
`link_spi.c` (`s_master_seen`, set from the driver's ISR on the first buffer
event). It keeps the line quiet during the keyboard's boot. It costs nothing and
it is not a security rule — degrading it would break nothing today — but it is
worth knowing that the line is *guaranteed* silent until you talk, so an IRQ
before your first transaction means something is wrong, not that the chest is
eager.

Historical note, in case you find it in our older documents: the IRQ was once
planned to land on S3 GPIO46, and there is an analysis in our spec explaining
why that strapping pin was safe. **That analysis is about a pin that is no
longer used** — the routed net is IO11. Our `board.h` comment still says GPIO46
and is stale on that point. If IO11 has a strapping role on your side, that
question is open and we have not answered it.

## 7. The shared bus, three slaves deep

`SCK` / `MISO` / `MOSI` (S3 IO38 / IO39 / IO40) carry three devices:

| slave | select | polarity |
|---|---|---|
| nRF24 | `CSN`, GPIO16 | active LOW |
| Sharp display | `LCD_CS`, GPIO14 | active **HIGH** |
| chest (P4) | `CS_P4`, IO7 | active LOW (section 3) |

Two consequences bear on both firmwares.

### The chest releases MISO when not selected

It must, and it does. The chest never configures MOSI, SCK or MISO as outputs —
at any moment, even transiently. Only the SPI slave driver touches them, and it
only drives them while CS is asserted ("is only active on the bus when the Host
asserts the Device's individual CS line", ESP-IDF `spi_slave.rst`). On any init
failure the chest puts all five pins back to **input, no pull**, and says so in
its log: a failure that left outputs driven would be worse than having no link
at all. The chest survives without the link; the keyboard does not survive a
nailed bus.

This is not theory. **On 2026-09-05, an unprogrammed P4 nailed SCK, MISO and
MOSI. The nRF24 went silent. It cost an hour of diagnosis, and the cause was
found last** — because the symptom was in the radio and the culprit was a board
that was not running any code at all. Worth remembering as a first hypothesis
the next time the radio goes quiet with a chest attached: a chest in download
mode, mid-flash, or with a firmware that failed early is exactly that situation
again.

### Nothing arbitrates between the three

There is no arbitration today, on either side. Three CS lines and a convention
that only one is asserted at a time — which holds only as long as all the code
that touches this bus goes through the same serialisation. That is the master's
call and it is entirely on your side of the wire (section 9); the chest has no
way to know it was selected while the radio was mid-transaction, and no way to
object.

## 8. Version policy

`LINK_PROTO_VERSION` is **1** and travels in the registers at `0x04`, in every
block, on every read. It is there because **the two repos will not always be
flashed together** — the chest is a separate board with a separate flashing
path, and the day one of them is a version ahead is not a hypothesis.

**The rule: a master that reads a version it does not know refuses, rather than
improvising.** No partial interpretation, no "the fields I recognise are
probably still there". The chest applies exactly that rule in the other
direction — `link_proto_parse_status()` rejects any version byte that is not its
own, before even checking the CRC. Vector V5 in section 10 is a version-2 block
with a *correct* CRC, which parses as `false`: valid, well-formed, and refused.

Practically, on the master side, "refuses" should mean: treat the chest as
unusable, log it once with both version numbers, and do not write the
confirmation byte. A chest whose protocol you do not understand is a chest whose
`0x10` may mean something else.

**What must change together** when the register map moves:

1. `LINK_PROTO_VERSION` in `main/link/link_proto.h` — always, even for an
   apparently backward-compatible addition. A new state bit is safe to ignore; a
   moved field is not, and only the version tells them apart.
2. The `LINK_REG_*` offsets and the two ownership ranges, keeping the
   word-alignment invariant (section 1). `link_spi.c`'s `_Static_assert`s will
   refuse the build otherwise — that is their whole job.
3. `LINK_REG_CRC_SPAN`, if any chest field is added or moved.
4. The host tests in `test/test_link_proto.c`.
5. **Section 10 of this document, regenerated by running the code**, and a note
   to KeSp. A contract whose vectors were edited by hand is worse than no
   contract.

Adding a **state bit** (`0x05` bits 3–7), or filling one of the chest's reserved
bytes at `0x0E`–`0x0F`, still requires a version bump by rule 1 — but a master
written to ignore unknown state bits will keep working, so the bump is cheap for
you. Adding a field to *your* reserved bytes `0x11`–`0x13` requires nothing from
us: the chest never reads or writes them, and the CRC does not cover them.

## 9. What is yours to decide

### 9.1 IRQ or polling

The wire is there and the chest drives it. **Whether you use it is your call**,
because the keyboard is the half that lives on battery and we are not the ones
paying for it. The chest supports both, and must keep supporting both.

- **Polling.** Read the 20-byte block on an interval and watch
  `pending_op`. Simplest; no interrupt handling; no dependence on IO11 being
  the pin we think it is (section 2). Costs a transaction per interval, on a
  bus you share with the radio, and adds up to half your interval to the
  latency the owner perceives. The chest refreshes its block every 20 ms, so
  polling faster than that gains nothing.
- **Interrupt.** A rising edge on IO11 means a confirmation is pending. Cheapest
  at rest — nothing on the bus while the chest has nothing to ask. Requires the
  pull-down (section 6), requires IO11 to be verified, and requires you to
  handle the case where the chest disappears while the line is asserted (it goes
  high-impedance, the pull-down releases it; there is no "cancel" message).
- **Both** is reasonable: interrupt-driven with a slow polling floor, which
  costs little and does not depend on IO11 being right. It also degrades into
  pure polling if the IRQ turns out not to be routed where we think, which is
  the honest hedge given section 2.

Either way, the confirmation byte is written the same way, and nothing in
sections 1 and 5 changes.

### 9.2 Confirm the line parameters

Everything in section 3 marked *chosen* is a default we picked to compile.
Specifically, please confirm or reject:

- **CS active low** — the one that fails silently (section 3). Highest priority.
- **Mode 0** — we believe your `GPIO45` strap forces it, but the conclusion is
  about your board.
- **MSB first**, 8/8/8 command/address/dummy, 1-bit data width.
- **The dummy phase on writes** — whether your half-duplex master sends one.

### 9.3 Bus arbitration

Three slaves, no arbiter (section 7). Deciding how the nRF24, the display and
the chest take turns is a master-side decision; the chest has no say and no
visibility. What we would ask is only this: the chest's register block is
self-consistent per read, but a read *interrupted* halfway will fail its CRC
rather than return a torn value — so a fair arbiter is enough, no locking
protocol required.

### 9.4 The security invariant — yours, and it is the point of the link

**The S3 writes a confirmation only on a real key press.** Never from the CDC
protocol, never from anything the host can reach.

This lives entirely in `KeSp_firmware` and we cannot enforce it from here, but
it is the reason the link exists. The chest has no source of physical presence
of its own — no button, no contact — and its only other channel is USB, which is
precisely the adversary in the threat model. The OpenPGP stack's strength rests
on `sec_confirm`: a press that host malware cannot fabricate. The PIN can be
captured; the press cannot. If a confirmation can be produced by anything the
host can reach, that lock is reopened and the link is worse than useless — it
becomes a remote-controlled "yes".

It deserves a `check.sh`-style rule on your side, the way our hardware
guardrails work here.

## 10. Test vectors

Prose is read once and ages. These are the contract's teeth: run them through
your parser, and a divergence goes red on whichever side deviates.

**How these were produced**: by compiling a throwaway host program against the
actual `main/link/link_proto.c` and `main/sys/cr_crc16.c`, building each block
and printing what `link_proto_is_absent()` and `link_proto_parse_status()`
return for it. Not written by hand. They are also pinned in
`test/test_link_proto.c` (`test_shared_vectors_*`), so the fast check turns red
here if the chest's behaviour ever stops matching this table.

All blocks are 20 bytes, in offset order `0x00` → `0x13`.

| # | bytes |
|---|---|
| V1 | `4E 49 50 48 01 07 01 00 2A 00 00 00 AF EA 00 00 00 00 00 00` |
| V2 | `00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00` |
| V3 | `FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF` |
| V4 | `4E 49 50 58 01 07 01 00 2A 00 00 00 AF EA 00 00 00 00 00 00` |
| V5 | `4E 49 50 48 02 07 01 00 2A 00 00 00 7F 60 00 00 00 00 00 00` |
| V6 | `4E 49 50 48 01 07 01 00 2B 00 00 00 AF EA 00 00 00 00 00 00` |
| V6b | `4E 49 50 48 01 07 01 00 2A 00 00 00 AE EA 00 00 00 00 00 00` |
| V7 | `4E 49 50 48 01 07 01 00 2A 00 00 00 AF EA 00 00 00 00 00` *(19 bytes)* |
| V8 | `4E 49 50 48 01 07 01 00 2A 00 00 00 AF EA 00 00 5A 00 00 00` |
| V9 | `4E 49 50 48 01 00 00 00 00 00 00 00 61 7A 00 00 00 00 00 00` |

| # | what it is | `is_absent` | `parse` | decoded |
|---|---|---|---|---|
| V1 | nominal: SD present + USB mounted + ready, PSO:CDS pending, 42 confirmations | `false` | **`true`** | version 1, state `0x07`, pending_op 1, count 42 |
| V2 | chest absent, line reads `0x00` | **`true`** | `false` | — |
| V3 | chest absent, line reads `0xFF` | **`true`** | `false` | — |
| V4 | bad magic word — one byte, `NIPH` → `NIPX`; everything else is V1 | `false` | `false` | — |
| V5 | unknown version: a chest announcing protocol 2, **CRC recomputed and correct** | `false` | `false` | — |
| V6 | CRC wrong by one bit: `confirm_count` LSB flipped (42 → 43), CRC left stale | `false` | `false` | — |
| V6b | CRC wrong by one bit, the other way: payload intact, one bit flipped *in the CRC field* | `false` | `false` | — |
| V7 | truncated: V1's first 19 bytes, `len = 19` | `false` | `false` | — |
| V8 | V1 with **only the master's byte changed** — a confirmation the chest has not yet read | `false` | **`true`** | identical to V1: version 1, state `0x07`, pending_op 1, count 42 |
| V9 | chest present and **not ready**: no state bits, nothing pending, counter zero | `false` | **`true`** | version 1, state `0x00`, pending_op 0, count 0 |

Reading notes, since these are the cases that catch a wrong implementation:

- **V1 vs V8** — the two blocks differ only at offset `0x10`, and the CRC bytes
  are identical (`AF EA`) in both. That is the CRC span made visible: a master
  that recomputes over 20 bytes instead of 12 will accept V1 and reject V8, and
  will therefore reject the block exactly whenever a confirmation is in flight.
- **V5** — well-formed, correct CRC, refused on the version byte alone. If your
  parser accepts V5, it will one day misread a chest that is a version ahead.
- **V2 and V3** — `parse` returns `false`, but the reason is *absent*, not
  *invalid*. A master that logs these as errors will log them constantly, since
  this is the chest's ordinary state.
- **V9** — valid and useful: it says "there is a chest here, it is booting". A
  master that treats an all-zero state byte as "nothing there" loses that
  distinction. Note that V9's CRC (`61 7A`) is not zero, which is what separates
  it from V2.
- **V6b** — a master that only checks "does the payload hash to something"
  without comparing against the stored bytes would pass this one.

## 11. Where the code and the design spec disagree

Found while writing this document. In each case the code is what ships, and this
contract follows the code. Listed so that nobody reconciles the spec instead of
the implementation.

1. **The S3 pinout.** The spec's §3 table says CS = GPIO3, IRQ = GPIO46. Both
   are obsolete and both are wrong: the routed nets are IO7 and IO11
   (section 2). The spec's whole sub-section arguing that GPIO46's strapping
   role is harmless is therefore an analysis of a pin that is not used.
2. **`board.h` carries the same stale value.** The comment on
   `BOARD_LINK_IRQ` still reads *"Côté clavier ce signal arrive sur GPIO46"* and
   reproduces the strapping analysis. The `#define` itself is the P4's GPIO11
   and is correct; only the prose about the far end is stale.
3. **The framed data channel does not exist.** The spec specifies a frame format
   (`SOF 0xA5`, version, opcode, length, payload, CRC16) and calls it "not
   implemented in this increment". It is still not implemented: there is no
   framing function in `link_proto.{c,h}`, and `link_spi.c` sets
   `max_transfer_sz = 0` with `queue_size = 1`. The shared registers are the
   entire protocol today.
4. **"Unknown version: frame rejected, counter incremented."** The spec's §6
   promises a rejection counter. There is none, anywhere. Rejections are
   silent apart from the unexpected-confirmation-byte log.
5. **The CRC's name.** `main/sys/cr_crc16.h` documents the function as
   "CRC-16/X-25". The implementation has no final XOR, which makes it
   CRC-16/MCRF4XX; its check value over `"123456789"` is `0x6F91`, where X-25
   would give `0x906E`. The *function* is the one we believe you already have —
   only the label is wrong. Compare check values, not names.
6. **The register map in the spec is correct** and matches `link_proto.h`
   offset for offset, including the word-ownership argument. It was revised
   once, and the revision landed in both.
