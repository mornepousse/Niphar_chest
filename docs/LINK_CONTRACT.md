# S3↔chest link — protocol contract

**Audience**: the `KeSp_firmware` team, writing the master side.
**Authority**: this document. The chest is the SPI *slave* and publishes the
register map, so the protocol is defined here and consumed there.
**Written on**: 2026-09-29, from the implementation, not from the design spec.
**Protocol version**: **3** (see section 9). Versions 1 and 2 are superseded;
mismatched halves refuse each other by design, on the version byte.

**What v3 adds, and why**: the chest now publishes the **name of the account**
an operation targets, so your screen can show *which* account the owner is
approving rather than only *what kind* of operation is pending. It also opens a
**second channel** (section 13) so the keyboard can browse accounts and display
TOTP codes without the host ever seeing them, and carries a **wall-clock time**
that the host sets once per plug-in (section 14).

The chest side is implemented, builds, and **runs on hardware**. The bytes below
are what the chest's code actually produces and accepts, and section 11 gives you
the vectors to check it against.

**The electrical side is no longer the open question — 2026-09-29.** It was, in
every earlier revision of this document. Two independent observations closed it:

- the chest counts master accesses to its shared buffer, from the driver's ISR
  (`SPI_EV_BUF_TX`/`SPI_EV_BUF_RX`, master activity only). Sampled inside one
  boot: 8 → 40 → 72, a steady **4 Hz** — your 250 ms polling interval. Your
  master issues **well-formed** shared-buffer commands, continuously. That
  implicitly validates **CS active low** (the one that was to fail silently),
  **mode 0**, bit order, the command/address/dummy shape, and the wiring on
  GPIO7–10;
- your left half displays `P4?`, a state your v1 parser reaches **only** when
  the magic word is right and the version differs from 1. So `NIPH` and the
  version byte cross the wire **intact**, and the refusal happens for the
  correct reason.

**And the whole block now crosses intact.** A read-only v3 master on the KeSp
bench reads the sixty-four bytes and accepts them only after checking the magic
word, version 3, the **CRC over `0x00`–`0x35`**, `label_len ≤ 34` and the
“mounted” invariant — then displays `P4` and `SD` from state byte `0x05` alone.
Every field, the CRC included, arrives intact and is verified by an
implementation that is not ours. Transfer length, the dummy phase on reads and
the byte order of every multi-byte field are settled at the frame level, not by
inference.

**What remains unproven**, and it is now two things, both in the *master→chest*
direction:

- **`WRBUF`** — the confirmation `{5A, instance}` at `0x38`–`0x39`, the requested
  mode at `0x3A`, the doorbell at `0x3C`. Nothing has been written to the chest
  yet; it has only been read;
- **the whole DMA channel** of section 13. Its receive is armed at init and
  segments are queued, but no frame has crossed it.

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
wins; section 12 lists every disagreement we found while writing this.

## 1. The register map

Sixty-four bytes, sixteen 32-bit words, in the `spi_slave_hd` **shared
register** buffer. They are readable and writable by the master *without the chest's
firmware having prepared anything* — the SPI peripheral answers on its own.
That is deliberate: it means you can still read the chest's state while its
firmware is hung, and it is why we chose `spi_slave_hd` over the classic slave
driver (which would require the slave to post a buffer before every
transaction, and hence a "ready" line we do not have).

| offset | word | size | field | owner | notes |
|---|---|---|---|---|---|
| `0x00`–`0x03` | 0 | 4 | magic word `NIPH` | chest→S3 | `4E 49 50 48`, in that byte order |
| `0x04` | 1 | 1 | protocol version | chest→S3 | **`0x03`** |
| `0x05` | 1 | 1 | state bits | chest→S3 | see below |
| `0x06`–`0x07` | 1 | 2 | pending operation | chest→S3 | little-endian, `0` = none armed |
| `0x08`–`0x0B` | 2 | 4 | consumed-confirmation counter | chest→S3 | little-endian, free-running |
| `0x0C` | 3 | 1 | instance number of the armed operation | chest→S3 | section 5 |
| `0x0D` | 3 | 1 | active USB mode (wire value) | chest→S3 | section 1, “Active USB mode” |
| **`0x0E`** | 3 | 1 | **label length** | chest→S3 | **new in v3**, `0`–`34` |
| **`0x0F`** | 3 | 1 | **accounts targeted** | chest→S3 | **new in v3**, `0`/`1`/`N` |
| **`0x10`** | 4 | 1 | **queued segment kind** | chest→S3 | **new in v3**, `0` = nothing queued |
| **`0x11`** | 4 | 1 | **segment number** | chest→S3 | **new in v3**, bumped after queueing |
| **`0x12`–`0x13`** | 4 | 2 | **segment length** | chest→S3 | **new in v3**, little-endian |
| **`0x14`–`0x35`** | 5–13 | 34 | **label**, printable ASCII | chest→S3 | **new in v3**, NOT NUL-terminated |
| **`0x36`–`0x37`** | 13 | 2 | **CRC16 over `0x00`–`0x35`** | chest→S3 | little-endian — **moved in v3** |
| **`0x38`** | 14 | 1 | user confirmation | S3→chest | section 5 — **moved from `0x10`** |
| **`0x39`** | 14 | 1 | echo of the instance number | S3→chest | section 5 — **moved from `0x11`** |
| **`0x3A`** | 14 | 1 | requested USB mode | S3→chest | section 6 — **moved from `0x12`** |
| `0x3B` | 14 | 1 | reserved | S3→chest | **yours** |
| **`0x3C`** | 15 | 1 | **request doorbell** | S3→chest | **new in v3**, section 13 |
| `0x3D`–`0x3F` | 15 | 3 | reserved | S3→chest | **yours** |

**Sixty-four bytes, sixteen words** — the whole of the P4's shared register file
(`SOC_SPI_MAXIMUM_BUFFER_SIZE`, `components/soc/esp32p4/include/soc/soc_caps.h:574`).
Chest `0x00`–`0x37` (fourteen whole words), master `0x38`–`0x3F` (two), **no word
shared**. There is no room left: the next chest-side field is a v4.

### The doorbell is in the master's SECOND word, and that is not cosmetic

Your team found this, and the code confirms it. When the chest clears a consumed
confirmation byte it calls `spi_slave_hd_write_buffer()` for **one** byte — but
the driver writes **by 32-bit words**, so that is a read-modify-write over the
master's *first* word. A doorbell living in that word would be **overwritten**
whenever it landed in those few cycles, and the request would never be served,
with no error anywhere.

This is the same window section 5 already describes for the mode byte. The
difference is decisive: the mode is re-read and re-written every cycle, so it
heals. **A doorbell is never re-read.** Hence `0x3C`, in the word the chest
never writes.


### What changed in v2, and why it could not wait

Two defects, one of them blocking. Both were free to fix only as long as your
master was not yet flashed.

**1 — The chest was inert, and that is the blocking one.** V1 carried the state,
the pending operation, the counter and the confirmation, and **no field with
which to ask for a USB mode**. The chest's console cannot stand in for it
either: `boards/niphar_chest/board.h` sets `BOARD_CONSOLE_ACTIONS 0`, so the
selector command is not even compiled into its firmware. The chest therefore
booted into `USB_MODE_NONE` and **nothing could take it out** — microSD
answering (`sd info` reports a 30 528 MB SDHC), applets present, link working,
and a host that never saw a thing. V1 had handled *presence*; nobody had carried
over *selection*. Byte `0x3A` is that field; section 6 is its whole semantics.

**2 — Confirmation resumption keyed on the operation code, not on the
instance.** Your team found this one. If your first write is lost and an
operation with the **same code** is armed within 200 ms, your retry confirms it.
Concretely: the owner presses for “CODE OTP GITHUB”, the write is lost, the
operation expires, the host arms one for “CODE OTP BANQUE”, the retry confirms
it. She never consented to that account — and the screen, which exists precisely
so that her press means something, was showing her the other one. Bytes `0x0C`
and `0x39` are that fix, and section 5 is the rule.

### The CRC moved back to `0x0E`, and that is not a reversal

Read this before concluding that we undid the word-separation fix, because the
offset alone makes it look that way.

The map that predates the separation *did* put the CRC at `0x0E`. But the
master's byte was then at `0x0C` — **in the same 32-bit word**. That was the
defect: not the offset, the shared word. The master now lives in `0x38`–`0x3F`
and the chest owns `0x00`–`0x0F` whole. No word is shared, the argument in “Why
five words, and no word shared” below still holds unchanged, and the CRC is free
to cover everything that precedes it **contiguously**.

That contiguity is the point. Two other placements for the instance number were
considered and rejected:

- **Instance at `0x0E`, CRC left at `0x0C`–`0x0D`.** The covered span becomes
  **discontiguous** — `0x00`–`0x0B`, then `0x0E`. Both implementations would
  have to reproduce exactly the same skip, and whichever one got it wrong would
  see every single block refused, with nothing anywhere to say why.
- **Instance outside the CRC.** A flipped bit on it breaks nothing serious, but
  it produces a refused confirmation that neither side can account for. The CRC
  exists so that “it is corrupt” stays an available answer.

There is a second, sharper reason to cover it. The chest publishes its zone in
one write, but you may be reading during it. Without CRC coverage of `0x0C`, a
**torn** read — the operation from one arming paired with the instance of the
next — would pass the CRC, and you would display one operation while echoing
another's instance. That is the exact defect the instance number exists to
close, reintroduced by the transport. Vector V6d in section 11 is that block.

**Established.** The offsets are `LINK_REG_*` in `link_proto.h`. The two
ownership ranges are `LINK_REG_CHEST_BASE`/`_LEN` = `0x00`/`0x38` and
`LINK_REG_MASTER_BASE`/`_LEN` = `0x38`/`0x08`, widened by v3 — the block is
still twenty bytes and the two ends still share no word.

### State bits (offset `0x05`)

Bit 3, **`TIME_VALID`**, is new in v3: the chest holds a wall-clock time that was
**set**, never guessed. See section 14 — and note that it never rises on a
default, an epoch or a zero.


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

### Instance number (`0x0C`) — new in v2

**One byte. It identifies the *arming*, not the operation.**

Two consecutive OpenPGP signatures, or two “CODE OTP” for two different
accounts, carry the same code at `0x06`–`0x07`, often the same label, and can
be armed inside the same millisecond. Nothing in v1 separated them. The instance
number does: it is the low byte of a counter the chest increments on **every**
arming, whatever the operation is.

Read it in the same transaction as the pending operation — they are both in the
chest's zone and the CRC covers both, so one read gives you a pair that is
consistent or refused, never silently mismatched. Show the operation to the
owner, and when she presses, echo **that** instance back at `0x39`.

It is not a nonce and it is not a secret: it is a discriminator. It wraps at
256. What that buys, stated as a limit rather than as a guarantee: for a value
to come round again *while an echo can still arrive*, 256 armings must happen
inside one press window — bounded by the chest's 15-second confirmation timeout,
so roughly seventeen armings per second, each overwriting the last under the
owner's eyes. The v1 defect needed **two**. A byte is what the layout affords
and this is what it is worth.

`0x0C` is `0x00` on a chest that has never armed anything (vector V9).

### Active USB mode (`0x0D`) — added after v2 was first published

**The byte you asked us not to leave reserved.** It carries the wire value of
the mode the chest has **actually installed**, using the same numbering as the
request at `0x3A` (section 8).

Until this byte existed, the protocol had no readback of the mode at all.
`USB_MOUNTED` says *something* is mounted, never *what*. Your screen could
therefore only ever show the mode it had **requested** — and requested and
actual are indistinguishable while everything works, then diverge exactly when
it does not: a switch that fails and is retried. The owner hit that symptom on
our side the same day (an SD card answering, a screen saying so, and nothing on
the host, because the chest had stayed in `none` and nothing said so). On your
side it would have been worse: no console to contradict the screen.

**`0xFF` means indeterminate**, and it is the only value here that is not a
mode. The chest publishes it while a switch is in flight — that is, whenever its
own `usb_mode_is_known()` is false. The rule is inherited from the byte next
door: `USB_MOUNTED` already refuses to count an uncertain mode, because what the
host sees is not guaranteed then and announcing it would be a lie. Publishing
the current mode mid-switch would be exactly that lie, during the one second it
matters. `0xFF` is outside the contiguous mode range, so a master comparing
`0x0D` against what it requested cannot hit an accidental equality on the way.

**What you can rely on:**

- `0x0D == 0x3A` → the chest is in the mode you asked for. This is the steady
  state.
- `0x0D == 0xFF` → a switch is in flight. Show the requested mode as pending;
  do not report a failure. A switch can legitimately take **up to ~15 s**
  (section 5's timeout bounds the same worst case: `usb_mode_apply_wire()` tears
  down the current descriptors and installs the next).
- `0x0D != 0x3A`, both known values, **persisting across several reads** → the
  chest refused or failed the switch and is still in the previous mode. Rewrite
  `0x3A`; if it does not move, that is a real fault and worth surfacing.
- `USB_MOUNTED` set → `0x0D` is a known value other than `0x00`. The two
  describe the same fact and may not contradict each other; a block that did
  would be one neither side can arbitrate. Vector V14 is the in-flight case,
  V1 the mounted one.

It is inside the CRC span, like the instance number and for the same reason — a
flipped bit here would otherwise make your screen name a mode the chest never
had, which is precisely what the byte exists to prevent. Vector **V6e** proves
it in bytes.

### The CRC, and why it stops where it does

**Established.** CRC16 over exactly `0x00`–`0x0D` (14 bytes,
`LINK_REG_CRC_SPAN`), stored little-endian at `0x0E`–`0x0F`. Contiguous: every
byte before the CRC field, no gaps, no skips.

Algorithm — reflected, polynomial `0x8408` (i.e. `0x1021` reflected), init
`0xFFFF`, **no final XOR**. Check value over the ASCII string `"123456789"` is
**`0x6F91`**. That is CRC-16/MCRF4XX in the catalogue's naming. It is
`cr_crc16()` in `main/sys/cr_crc16.c`, which our repo records as taken verbatim
from `KeSp_firmware/main/security/cr_crc16.c` — so you should already have the
identical function. Please check the check value against yours rather than
against the name: our own header comment calls it "CRC-16/X-25", which is wrong
(X-25 XORs `0xFFFF` at the end and checks as `0x906E`). The code, not the
comment, is what runs.

**Why the CRC stops at `0x0D` and does not cover the master's word.** If it
covered `0x38`–`0x3F`, then every legitimate write of yours — putting `0x5A` in
the confirmation byte, or a mode in `0x3A` — would make the block fail its own
CRC, on both sides, for the entire time the byte sits there. The chest would be publishing a block
that reads as corrupt precisely when something is happening. So the CRC is a
chest→S3 integrity check on chest→S3 fields only. Your word is outside it, and
you may put whatever you like in `0x39`–`0x3F` later without invalidating
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
  touching `0x38`–`0x3F` (v2 adds the instance number inside that same write —
  it did not change the shape of it);
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

### S3 side — verified against the exported netlist, 2026-09-29

| net | S3 (U6) | pull | shared with |
|---|---|---|---|
| `CS_P4` | **GPIO3** (pin 15) | R48 10 kΩ → `P4_3V3` | nobody (one pin per slave) |
| `IRQ_P4` | **GPIO46** (pin 16) | R49 10 kΩ → GND | nobody |
| `SCK` | IO38 | — | nRF24, Sharp display |
| `MISO` | IO39 | — | nRF24, Sharp display |
| `MOSI` | IO40 | — | nRF24, Sharp display |

> **Correction — this section said IO7 / IO11 until 2026-09-29, and that was
> wrong.**
>
> Those are the **chest-side** numbers, reported by mistake onto the S3 column.
> The KeSp session exported the netlist and settled it:
>
> ```
> kicad-cli sch export netlist --format kicadsexpr \
>   -o niphar.net Niphargus/hardware/pcb/niphar.kicad_sch
>
> net CS_P4  : U6 pin 15 "GPIO3/TOUCH3/ADC1_CH2_15"  +  U16 pin 11 "GPIO7_11"
> net IRQ_P4 : U6 pin 16 "GPIO46_16"                 +  U16 pin 15 "GPIO11_15"
> S3 GPIO7 carries /s3/col2 — a matrix column, not the CS.
> ```
>
> We reproduced that export independently before correcting. **The design spec's
> §3 table was right all along**, and so was the strapping analysis in our
> `boards/niphar_chest/board.h`: GPIO46 *is* the routed pin, and the reasoning
> that shows it harmless applies.
>
> Two upstream causes, one on each side. Yours:
> `NIPHARGUS_V2_HARDWARE.md` classed GPIO3 and GPIO46 among the *unwired* pins,
> with a test that enforces it — which is what made an S3 assignment on those
> pins look impossible. Ours: a number given in conversation was taken for a
> routing, even after noticing it matched the chest's own pins exactly.
>
> **A pinout is settled by the netlist, not by memory** — including the memory
> of whoever drew the board.

### The pull-up on `CS_P4` is on the chest's rail

R48 ties `CS_P4` to **`P4_3V3`**, not to the keyboard's rail. Two consequences
on your side:

- Driving GPIO3 while the chest is unpowered pushes roughly 0.33 mA into a dead
  rail, through the P4's protection diodes. This is the same reasoning that made
  the IRQ active-HIGH with a pull-down (section 6) — the chest is unpowered most
  of the time, since it only wakes when wired.
- When you leave the line alone, R48 holds the chest **deselected**. That is the
  safe default, and it means an undriven CS is not an ambiguous state.

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
CMD 0x02 (RDBUF) | ADDR 0x00 | 8 dummy bits | 64 bytes out (slave→master)
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
section 11 is exactly that block.

## 5. The confirmation, and the instance it belongs to

**A confirmation is accepted if and only if `0x38` is `0x5A` **and** `0x39` is
the instance number the chest currently has armed.** Anything else is ignored,
and the ignoring is silent.

That conjunction is the whole of the v2 change. V1 had only the first half, and
the second half is what ties a press to a *moment* rather than to a *kind of
operation*.

### The handshake, from your side

1. Read the block. See `pending_op ≠ 0` (and `READY` set). **Note the instance
   number at `0x0C` from that same read.**
2. Show the operation to the owner and obtain a **real key press**.
3. Write `0x5A` to `0x38` **and the noted instance to `0x39`**. Both bytes, in
   one write if you can — they are in your word and nothing of ours is in it.
4. Poll the block. The confirmation counter increments → accepted and relayed.
   The pending operation clears → the chest acted on it.

If the counter does **not** move, go back to step 1: re-read the block, take the
current instance, and write again. Do not reuse the instance you noted before —
that is exactly the mistake this field exists to make impossible.

### What the chest does, byte by byte

The chest polls `0x38`–`0x3F` every 20 ms, in one read. On any non-zero value at
`0x38` it **immediately writes `0x00` back**, before deciding anything, and
then:

- `0x38 == 0x5A` **and** `0x39 == armed instance` → a real press for the
  operation we armed. Relayed to the security layer; the confirmation counter at
  `0x08` increments.
- `0x38 == 0x5A`, `0x39` ≠ armed instance → **ignored**. The counter does not
  move. Logged at INFO on our side, because this is *not* an anomaly: one lost
  write plus one expiry is enough to produce it.
- `0x38` non-zero and not `0x5A` → discarded, logged as
  `octet de confirmation inattendu 0x..`. The counter does not move.

**Reclaiming the byte happens in all three cases, and it has to.** In v1 the
reason was replay: a `0x5A` left in place would be re-read every 20 ms and
replayed against every operation armed afterwards. In v2 there is a sharper
reason — an echo that matches nothing *today* would match a **future** instance
after a few armings. Leaving the byte in place would turn a stale press into a
deferred one. One gesture, one authorisation.

### “Silent” means silent toward you, and the counter is the whole signal

There is no error code, no status field, no rejection counter. There is nowhere
to put one: the chest's zone is full and adding a field would change a layout
you already hold. So a refused confirmation looks exactly like a lost one — the
counter does not move — and the correct response to both is the same: re-read,
re-echo, rewrite. That symmetry is deliberate, and it is why we did not try to
distinguish them.

We do log it on our side, at INFO for a stale echo and WARN for a malformed
byte. Silence toward the master is a protocol decision; silence in the journal
would just make the link undebuggable.

### A confirmation with nothing armed

Not an error. The chest publishes the **last** instance it armed at `0x0C`, even
once that operation has been consumed or has expired. An echo matching it is
therefore accepted, relayed, and counted — and the security layer, which has
nothing pending, does nothing with it. That keeps the counter's meaning intact
(“my write arrived”) without granting anything. A press can never be banked for
the next operation: `sec_confirm` only ever grants what is armed at the instant
the press is relayed.

### Why `0x5A` and not `1`

Because noise on a bus produces `1`, and does not produce `0x5A`.

`0x00` and `0xFF` are what a floating line reads as, so neither can be the
signal. A single stuck or glitching bit on a line at rest yields `0x01`, `0x02`,
`0x80` — any of the powers of two. `0x5A` is `0b01011010`: an alternating
pattern, five bit positions away from `0x00` and three from `0xFF`, reachable
from neither by one accident. It is not a checksum and it is not security — the
security invariant lives on your side, not ours (section 10) — it is a cheap
filter that keeps an electrical fault from being read as a human gesture.

The instance echo does not replace it. The magic byte filters *noise*; the
instance filters *time*. Vector V11 in section 11 is a perfectly formed `0x5A`
with a stale echo, and it is refused.

### The read-modify-write window, and what v2 added to it

Step 3 is a write into your own word. The chest's reclaim in step 4 is a
single-byte write *into your word*, which the driver performs as a
read-modify-write — so a second press landing in those few cycles would be lost.
That window only opens immediately after a press already received, at the moment
you are waiting to see the counter move, and a second press there would be a
duplicate anyway.

**V2 widens what that window can swallow, and you should know it.** The reclaim
rewrites your whole 32-bit word, so a **mode request** you write into `0x3A`
during those few cycles can be written back stale and lost. It is narrow (it
only opens right after a confirmation) and it is not dangerous (nothing is
exposed that was not already), but unlike a lost press it is **silent**: there
is no counter for mode changes. The mitigation is on your side and it is one
line — **re-read `0x3A` after writing it, and rewrite if it does not stick.**
Your polling loop already does this if it compares `0x3A` against the mode it
wants on every cycle, which is what section 6 asks for anyway.

We could not close it without writing one of your reserved bytes, which would set
a trap for the first field you put there.

## 6. Selecting the USB mode — new in v2

**Byte `0x3A`. This is the field that makes the chest usable at all.**

Without it the chest boots into `USB_MODE_NONE`, exposes no descriptors, and
nothing anywhere can change that: its console is compiled without the selector
command (`BOARD_CONSOLE_ACTIONS 0`) and it has no button. A chest that has just
been flashed shows up in no `lsusb`, no `lsblk`, no `gpg --card-status`, and
that is the *normal* state — “lots of things, one at a time”, never two at once.
`0x3A` is how one of them gets chosen.

### 6.1 The wire values

**Frozen by this contract, and deliberately independent of our internal
enumeration.**

| value | mode | what the host sees |
|---|---|---|
| `0x00` | none | nothing at all — the idle state |
| `0x01` | storage | the microSD as a mass-storage device |
| `0x02` | pgp | an OpenPGP card over CCID |
| `0x03` | otp | a CR-HMAC key over HID |
| `0x04` | fido | a U2F/CTAP-HID authenticator |
| `0x05` | oath | YKOATH TOTP accounts over CCID |

They happen to coincide with our `usb_mode_t` today. **Do not rely on that.**
Somebody will reorder that enum one day — to insert a mode, to group the two
CCID personalities — and the wire must not move with it. The translation lives
in `main/usb/usb_mode_wire.h` behind a two-way round-trip test that goes red the
moment either numbering drifts without the other.

**Any other value is refused.** The chest stays in the mode it is in and logs
the refusal once per distinct value. Never a fallback to `none` — that would
tear down the interface the owner is using in response to a byte we did not
understand — and never “the nearest one”.

### 6.2 Applied on change, against the last *applied* value

The chest acts when the byte **changes**, not on every read: it reads `0x3A`
twenty times a second and re-enumerating that often would be absurd.

The reference it compares against is **the last value it actually applied**,
starting at `0x00` on boot — not the last value it read. That distinction is
what makes the selection self-healing, and it is what you asked for:

- the chest reboots; its shared buffer comes back all zeros, so `0x3A` reads
  `0x00` and the chest's reference is `0x00`;
- your master, which re-reads `0x3A` every cycle and rewrites it whenever it
  differs from the mode it wants, sees `0x00` where it wanted `0x02` and writes
  `0x02`;
- the chest sees a genuine `0 → 2` change and applies it.

Nothing had to detect the reboot. **Re-read and rewrite on every cycle** is the
whole master-side algorithm, and it also covers the read-modify-write window
described at the end of section 5.

A switch that fails — `ESP_ERR_INVALID_STATE` because another switch is already
running, which can take up to fifteen seconds — does **not** update the chest's
reference. It is simply retried on the next tick. Again, nothing for you to
detect.

### 6.3 No physical confirmation for a mode change — an accepted limit, with its reasoning

**Changing mode requires no press.** That is the owner's decision, taken
deliberately: pressing a key on the keyboard *is* the gesture, and asking for a
second one to confirm the first would be theatre.

It has a consequence that deserves to be named rather than left for a reader to
work out. **`0x01 storage` exposes the owner's microSD as a mass-storage device
with nobody pressing anything**, and `0x02 pgp` loads her private-key state into
RAM. Whoever can write `0x3A` can therefore read her card.

This is acceptable, and here is why — not as a guarantee, as a reasoning:

- Writing `0x3A` means being **master of this SPI bus**, which means being
  **inside the left half of the keyboard**. Whoever is there already has the
  microSD in their hand. The link adds nothing to what they can do.
- The host the mode exposes something *to* is **the owner's own host**, through
  the USB of her own left half. It is not a third party.
- And above all: **exposing an applet is not authorising an operation.**
  Producing a code, deleting an account, signing — all of those still require
  the press *and* the instance echo of section 5. The mode opens the door; the
  confirmation guards what is behind it.

**The counterpart, stated honestly: a mode cannot be refused.** A master that
changed mode in a loop would keep the chest from being useful for anything —
every switch tears down the current interface before installing the next. That
is a denial of service, not a theft of secrets, and **the chest has no defence
against it**: it cannot tell a faulty master from an indecisive owner, and any
rate limit it imposed would be indistinguishable, in use, from a chest that
ignores her.

If that trade ever needs revisiting, `storage` is the candidate — it exposes
data without any operation being armed. It is a decision for the owner, not a
protocol change, and she has already taken it once.

### 6.4 What you should do with `0x3A`

1. Decide the mode you want (from a layer, a key, a menu — yours).
2. Every cycle, read the block. Compare `0x3A` against the mode you want.
3. If it differs, write your value to `0x3A`. One byte, in your own word.
4. Read the **active** mode at `0x0D` to see which mode is actually installed.
   `LINK_STATE_USB_MOUNTED` at `0x05` bit 1 still tells you mounted-or-not; the
   byte tells you *which*, and the two never contradict each other.
5. While `0x0D` is `0xFF`, a switch is in flight: show your request as pending,
   not as a failure. Only a disagreement between two **known** values, persisting
   across several reads, means the chest refused or failed the switch — then
   rewrite `0x3A`, and surface a fault if it still does not move.

This is the readback that the first published v2 did not have, and step 4 is the
reason it was added: without it, step 2's comparison is the only thing you can
show, which means your screen shows your own intent and calls it a state.

Vectors V12 (unknown value, refused), V13 (`0x02`, applied) and V14 (switch in
flight) in section 11 pin all three outcomes.

## 7. The interrupt line

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

**GPIO46 is a strapping pin on your side, and that is already answered.** The
IRQ lands on S3 GPIO46, and `boards/niphar_chest/board.h` carries the analysis:
that strap's only role is controlling ROM message printing on UART0, and with
`EFUSE_UART_PRINT_CONTROL` at its default value its level at reset is
explicitly "Ignored" (ESP32-S3 TRM v1.8, table 8.3-1, p. 536). Nothing to do
with GPIO45, which selects the flash rail voltage.

We hold the invariant **"never assert before the S3 has spoken at least once"**
anyway: it costs nothing, it keeps the line quiet through your boot, and if that
eFuse were ever changed, GPIO46 would regain a role at reset.

*(An earlier revision of this contract claimed the routed pin was IO11 and that
this analysis was obsolete. That was our error — see section 2.)*

## 8. The shared bus, three slaves deep

`SCK` / `MISO` / `MOSI` (S3 IO38 / IO39 / IO40) carry three devices:

| slave | select | polarity |
|---|---|---|
| nRF24 | `CSN`, GPIO16 | active LOW |
| Sharp display | `LCD_CS`, GPIO14 | active **HIGH** |
| chest (P4) | `CS_P4`, S3 GPIO3 | active LOW (section 3) |

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
call and it is entirely on your side of the wire (section 10); the chest has no
way to know it was selected while the radio was mid-transaction, and no way to
object.

## 9. Version policy

`LINK_PROTO_VERSION` is **2** and travels in the registers at `0x04`, in every
block, on every read. It is there because **the two repos will not always be
flashed together** — the chest is a separate board with a separate flashing
path, and the day one of them is a version ahead is not a hypothesis.

**The rule: a master that reads a version it does not know refuses, rather than
improvising.** No partial interpretation, no "the fields I recognise are
probably still there". The chest applies exactly that rule in the other
direction — `link_proto_parse_status()` rejects any version byte that is not its
own, before even checking the CRC. Vector V5 in section 11 is a version-3 block
with a *correct* CRC, which parses as `false`: valid, well-formed, and refused.

**This is not hypothetical any more: v1 exists in the field of our repository's
history, and v2 is the version you are reading.** A v1 master reading a v2 chest
refuses on the version byte, and a v2 master reading a v1 chest does the same.
That is the intended outcome — the two ends must be flashed together for this
change, and the version byte is what makes that failure loud instead of subtle.
Had we shipped v2's layout under version 1, a v1 master would have read the
CRC from `0x0C`–`0x0D`, found the instance number and a zero there, and rejected
every block for a reason it could not name.

Practically, on the master side, "refuses" should mean: treat the chest as
unusable, log it once with both version numbers, and do not write the
confirmation byte **or the mode byte**. A chest whose protocol you do not understand is a chest whose
`0x38` may mean something else.

**What must change together** when the register map moves:

1. `LINK_PROTO_VERSION` in `main/link/link_proto.h` — always, even for an
   apparently backward-compatible addition. A new state bit is safe to ignore; a
   moved field is not, and only the version tells them apart.
2. The `LINK_REG_*` offsets and the two ownership ranges, keeping the
   word-alignment invariant (section 1). `link_spi.c`'s `_Static_assert`s will
   refuse the build otherwise — that is their whole job.
3. `LINK_REG_CRC_SPAN`, if any chest field is added or moved.
4. The host tests in `test/test_link_proto.c`.
5. **Section 11 of this document, regenerated by running the code**, and a note
   to KeSp. A contract whose vectors were edited by hand is worse than no
   contract.

**v3 (2026-09-29)** moved every offset: the block went from 20 to 64 bytes, the
CRC to `0x36`–`0x37`, and your range to `0x38`–`0x3F`. No chest-side reserved byte
remains — the next field on our side is a v4. Your reserved bytes are `0x3B` and
`0x3D`–`0x3F`.

Adding a **state bit** (`0x05` bits 3–7) still requires a version bump by rule 1
— but a master written to ignore unknown state bits will keep working, so the
bump is cheap for you. The chest's last reserved byte, `0x0D`, was filled on
2026-09-29 by the active-mode readback **without** a version bump, and that was
a deliberate exception taken while no half had yet been flashed with v2: a
master that ignored the byte saw `0x00`, which was also the old reserved value,
so nothing in flight could misread it. That exception is spent — there is no
reserved byte left on our side, and the next chest-side field is a v3.
Adding a field to *your* reserved bytes `0x3B`, `0x3D`–`0x3F` requires nothing from us: the
chest never reads or writes it, and the CRC does not cover it.

Adding a **wire value for a new USB mode** (section 6.1) also requires a version
bump, and this one is not cheap for either of us: our `LINK_USB_MODE_COUNT` is
what makes an unknown value refusable, so a chest that knows `0x06` and a master
that does not, or the reverse, disagree about what is refusable. The values are
contiguous from zero by contract, precisely so that “known” is a comparison and
not a table to keep in step.

## 10. What is yours to decide

### 10.1 IRQ or polling

The wire is there and the chest drives it. **Whether you use it is your call**,
because the keyboard is the half that lives on battery and we are not the ones
paying for it. The chest supports both, and must keep supporting both.

- **Polling.** Read the 64-byte block on an interval and watch
  `pending_op`. Simplest; no interrupt handling; no dependence on GPIO46 being
  the pin we think it is (section 2). Costs a transaction per interval, on a
  bus you share with the radio, and adds up to half your interval to the
  latency the owner perceives. The chest refreshes its block every 20 ms, so
  polling faster than that gains nothing.
- **Interrupt.** A rising edge on GPIO46 means a confirmation is pending. Cheapest
  at rest — nothing on the bus while the chest has nothing to ask. Requires the
  pull-down (section 6), requires GPIO46 to be wired as expected, and requires you to
  handle the case where the chest disappears while the line is asserted (it goes
  high-impedance, the pull-down releases it; there is no "cancel" message).
- **Both** is reasonable: interrupt-driven with a slow polling floor, which
  costs little and does not depend on GPIO46 being right. It also degrades into
  pure polling if the IRQ turns out not to be routed where we think, which is
  the honest hedge given section 2.

Either way, the confirmation byte is written the same way, and nothing in
sections 1 and 5 changes.

### 10.2 Confirm the line parameters

Everything in section 3 marked *chosen* is a default we picked to compile.
Specifically, please confirm or reject:

- **CS active low** — the one that fails silently (section 3). Highest priority.
- **Mode 0** — we believe your `GPIO45` strap forces it, but the conclusion is
  about your board.
- **MSB first**, 8/8/8 command/address/dummy, 1-bit data width.
- **The dummy phase on writes** — whether your half-duplex master sends one.

### 10.3 Bus arbitration

Three slaves, no arbiter (section 7). Deciding how the nRF24, the display and
the chest take turns is a master-side decision; the chest has no say and no
visibility. What we would ask is only this: the chest's register block is
self-consistent per read, but a read *interrupted* halfway will fail its CRC
rather than return a torn value — so a fair arbiter is enough, no locking
protocol required.

### 10.4 The security invariant — yours, and it is the point of the link

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

## 11. Test vectors

Prose is read once and ages. These are the contract's teeth: run them through
your parser, and a divergence goes red on whichever side deviates.

**How these were produced**: by compiling a throwaway host program against the
actual `main/link/link_proto.c` and `main/sys/cr_crc16.c`, building each block
and printing what the decoders return for it. Not written by hand. They are also
pinned in `test/test_link_proto.c` (`test_shared_vectors_*`), so the fast check
turns red here if the chest's behaviour ever stops matching this table.

**Regenerated in full for v3** — every byte moved: the block went from 20 to 64
bytes, the version byte to `0x03`, the CRC to `0x36`–`0x37`, and the master's
range to `0x38`. Nothing from the v2 table survives.

**The operation codes are derived from `sec_op_t`, not transcribed** — and that
is a correction, not a precaution. An earlier revision of this table carried
`pending_op` `9` for V1 (which is `SEC_OP_OATH_REPLACE`, not a code request) and
`0x0C` for V16 (**which is not a value of the enum at all**). Both were numbers
picked by hand instead of derived, and the KeSp team found them by comparing the
table against the enum. The generator now includes `sec_confirm.h`, and the
pinned tests assert the *names*, so the two cannot drift again. Section 1 still
holds: **the enum is not frozen by this contract** — treat an unknown code as
“the chest is asking for a confirmation”.

The nominal block **V1** is an OATH code pending for **`GITHUB`**: SD present,
USB mounted, ready, **time valid**, 42 confirmations, **instance 3**, active mode
`oath`, one account targeted.

### Register blocks (64 bytes, offsets `0x00` → `0x3F`)

| # | bytes |
|---|---|
| V1 | `4E 49 50 48 03 0F 07 00 2A 00 00 00 03 05 06 01 00 00 00 00 47 49 54 48 55 42 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 F3 21 00 00 00 00 00 00 00 00` |
| V4 | `4E 49 50 58 03 0F 07 00 2A 00 00 00 03 05 06 01 00 00 00 00 47 49 54 48 55 42 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 F3 21 00 00 00 00 00 00 00 00` |
| V5 | `4E 49 50 48 04 0F 07 00 2A 00 00 00 03 05 06 01 00 00 00 00 47 49 54 48 55 42 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 6B D4 00 00 00 00 00 00 00 00` |
| V6 | `4E 49 50 48 03 0F 07 00 2B 00 00 00 03 05 06 01 00 00 00 00 47 49 54 48 55 42 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 F3 21 00 00 00 00 00 00 00 00` |
| V6B | `4E 49 50 48 03 0F 07 00 2A 00 00 00 03 05 06 01 00 00 00 00 47 49 54 48 55 42 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 F2 21 00 00 00 00 00 00 00 00` |
| V6C | `4E 49 50 48 03 0F 07 00 2A 00 00 00 03 05 06 01 00 00 00 00 47 49 54 48 55 42 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 F3 20 00 00 00 00 00 00 00 00` |
| V6D | `4E 49 50 48 03 0F 07 00 2A 00 00 00 02 05 06 01 00 00 00 00 47 49 54 48 55 42 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 F3 21 00 00 00 00 00 00 00 00` |
| V6E | `4E 49 50 48 03 0F 07 00 2A 00 00 00 03 01 06 01 00 00 00 00 47 49 54 48 55 42 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 F3 21 00 00 00 00 00 00 00 00` |
| V6F | `4E 49 50 48 03 0F 07 00 2A 00 00 00 03 05 06 01 00 00 00 00 67 49 54 48 55 42 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 F3 21 00 00 00 00 00 00 00 00` |
| V6G | `4E 49 50 48 03 0F 07 00 2A 00 00 00 03 05 23 01 00 00 00 00 47 49 54 48 55 42 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 70 D8 00 00 00 00 00 00 00 00` |
| V8 | `4E 49 50 48 03 0F 07 00 2A 00 00 00 03 05 06 01 00 00 00 00 47 49 54 48 55 42 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 F3 21 5A 03 00 00 00 00 00 00` |
| V9 | `4E 49 50 48 03 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 95 15 00 00 00 00 00 00 00 00` |
| V10 | `FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF 00` |
| V11 | `4E 49 50 48 03 0F 07 00 2A 00 00 00 03 05 06 01 00 00 00 00 47 49 54 48 55 42 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 F3 21 5A 02 00 00 00 00 00 00` |
| V12 | `4E 49 50 48 03 0F 07 00 2A 00 00 00 03 05 06 01 00 00 00 00 47 49 54 48 55 42 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 F3 21 00 00 09 00 00 00 00 00` |
| V13 | `4E 49 50 48 03 0F 07 00 2A 00 00 00 03 05 06 01 00 00 00 00 47 49 54 48 55 42 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 F3 21 00 00 02 00 00 00 00 00` |
| V14 | `4E 49 50 48 03 0D 07 00 2A 00 00 00 03 FF 06 01 00 00 00 00 47 49 54 48 55 42 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 30 15 00 00 00 00 00 00 00 00` |
| V15 | `4E 49 50 48 03 07 07 00 2A 00 00 00 03 05 06 01 00 00 00 00 47 49 54 48 55 42 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 15 F5 00 00 00 00 00 00 00 00` |
| V16 | `4E 49 50 48 03 0F 0A 00 2A 00 00 00 03 05 0A 0C 00 00 00 00 31 32 20 43 4F 4D 50 54 45 53 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 7A AF 00 00 00 00 00 00 00 00` |

| # | what it is | `is_absent` | `parse` | decoded |
|---|---|---|---|---|
| V1 | nominal: SD + mounted + ready + **time valid**, code pending for `GITHUB`, 42 confirmations, instance 3 | `false` | **`true`** | version 3, state `0x0F`, **pending_op 7 (`SEC_OP_OATH_CODE`)**, count 42, instance 3, active `0x05`, label `GITHUB`, op_count 1 |
| V2 | chest absent, line reads `0x00` (64 bytes) | **`true`** | `false` | — |
| V3 | chest absent, line reads `0xFF` (64 bytes) | **`true`** | `false` | — |
| V4 | bad magic word, `NIPH` → `NIPX`; everything else is V1 | `false` | `false` | — |
| V5 | unknown version: a chest announcing protocol **4**, **CRC recomputed and correct** | `false` | `false` | — |
| V6 | `confirm_count` LSB flipped (42 → 43), CRC left stale | `false` | `false` | — |
| V6b | payload intact, one bit flipped in the CRC field's **low** byte | `false` | `false` | — |
| V6c | the same, in the CRC field's **high** byte | `false` | `false` | — |
| V6d | **instance** changed (3 → 2), CRC left stale | `false` | `false` | — |
| V6e | **active mode** changed (`oath` → `storage`), CRC left stale | `false` | `false` | — |
| V6f | **one byte of the label** changed (`G` → `g`), CRC left stale — **the v3 vector you asked for** | `false` | `false` | — |
| V6g | **`label_len` = 35** for a 34-byte field, **CRC RECOMPUTED AND CORRECT** | `false` | `false` | — |
| V7 | truncated: V1's first 63 bytes, `len = 63` | `false` | `false` | — |
| V8 | V1 plus a confirmation not yet read, echoing the **armed** instance (3) at `0x39` | `false` | **`true`** | identical to V1; confirmation **accepted** |
| V9 | chest present and **not ready**: no state bits, nothing pending, no label, no time | `false` | **`true`** | version 3, everything zero |
| V10 | uniform `0xFF` except the **last** byte | `false` | `false` | — |
| V11 | V1 plus a well-formed `0x5A` echoing the **previous** instance (2) | `false` | **`true`** | block valid; confirmation **refused** |
| V12 | V1 plus an **unknown** mode request (`0x09`) at `0x3A` | `false` | **`true`** | block valid; mode request **refused** |
| V13 | V1 plus a valid mode request (`0x02`, pgp) at `0x3A` | `false` | **`true`** | block valid; mode request **applied** |
| V14 | switch in flight: active mode `0xFF`, `USB_MOUNTED` cleared | `false` | **`true`** | state `0x0D`, active **indeterminate** |
| V15 | mounted and ready but **no time set** (bit 3 clear) | `false` | **`true`** | state `0x07` — your **“NO TIME”** |
| V16 | a **RESET** pending: **`pending_op` `0x0A` (`SEC_OP_OATH_RESET`)**, `op_count` **12**, label `12 COMPTES` | `false` | **`true`** | twelve accounts on one press |

### DMA channel (section 13)

| # | bytes | what it is |
|---|---|---|
| R1 | `01 00 00 00 00 00 5B 0C` | request: `LIST` from index 0 |
| R2 | `02 05 00 00 00 00 72 26` | request: `CODE` for account 5 |
| R3 | `02 04 00 00 00 00 72 26` | R2 with the argument flipped one bit, CRC left — **refused** |
| L1 | `0C 03 00 01 00 06 47 49 54 48 55 42 01 09 4F 56 48 3A 50 45 52 53 4F 02 07 4F 56 48 3A 50 52 4F E5 D7` | `LIST` reply: 12 total, 3 in this page from index 0, **more** flag set — `GITHUB`, `OVH:PERSO`, `OVH:PRO` |
| C1 | `05 06 30 30 34 31 38 39 30 32 0C 00 8F 9B` | `CODE` reply: account 5, six digits, `00418902`, twelve seconds left |

Reading notes, since these are the cases that catch a wrong implementation:

- **V1 vs V8, V11, V12, V13** — all four differ from V1 only inside the master's
  range, and the CRC bytes are identical (`F3 21`) in all of them. That is the
  CRC span made visible: a parser that recomputes over 64 bytes instead of 54
  will accept V1 and reject the other four, and will therefore reject the block
  exactly whenever something is in flight.
- **V8 vs V11** — the two blocks differ in **one byte**, `0x39`: `03` against
  `02`. V8 is accepted, V11 is refused. If your implementation treats them the
  same, it has the v1 defect, and the “CODE OTP GITHUB → CODE OTP BANQUE” case is
  live on your side.
- **V6d, V6e and V6f together** — instance, active mode, label. Three covered
  fields, three single-byte changes with the CRC left alone, three rejections. If
  one of them passes, your CRC span is short by exactly that field.
- **V6g is the one that is not about corruption at all.** Its CRC is *correct*.
  The block is refused because a `label_len` of 35 cannot describe a 34-byte
  field — the refusal is about the block's **meaning**, not its transmission. If
  you truncate instead of refusing, you read bytes that are not the label and
  display a name the chest never composed, which is the exact thing this field
  exists to prevent. **Treat `label_len > 34` as a corrupt block.**

  **And the stronger reason, which came from the master side, not from here**:
  the KeSp team removed their own guard by mutation and found that a byte is
  then written **outside the buffer**. So this is not only about displaying the
  right name — it is memory safety on your side. Their argument stands on its
  own and can be verified; the display one asked you to take our word for it.
- **V15 and V16** — the two states v2 could not express. V15 is mounted, ready,
  and has no time: show “NO TIME”, do not ask for a code. V16 is a RESET with
  `op_count` 12: show the count, because one press destroys twelve secrets.
- **V6b and V6c together** — one bit in each half of the CRC field. A parser that
  only compares the low byte, or stores the CRC big-endian, passes one and fails
  the other. Neither alone is sufficient.
- **V5** — well-formed, correct CRC, refused on the version byte alone. If your
  parser accepts V5, it will one day misread a chest that is a version ahead.
- **V10** — sixty-three `0xFF` and one `0x00`. An absence test that stops at the
  first byte, or only sweeps the chest's zone, calls this “absent” and you lose a
  chest that is talking.
- **R3** — one bit in the request's *argument*, CRC left stale. Without the
  request CRC, a corrupted frame would arm a confirmation for an account nobody
  asked for. The owner would see an unexpected name and not press — the right
  outcome, but better not to get there.

**CRC check value**: `cr_crc16("123456789")` = **`0x6F91`**. Compare that number,
not the algorithm's name — our header says “CRC-16/X-25”, which would be
`0x906E`; the name is wrong, the function is not.

## 12. Where the code and the design spec disagree

Found while writing this document. In each case the code is what ships, and this
contract follows the code. Listed so that nobody reconciles the spec instead of
the implementation.

1. ~~**The S3 pinout.** The spec's §3 table says CS = GPIO3, IRQ = GPIO46. Both
   are obsolete and both are wrong.~~ **Retracted 2026-09-29 — the spec was
   right.** The netlist gives S3 GPIO3 for `CS_P4` and S3 GPIO46 for `IRQ_P4`
   (section 2). It was this contract that was wrong, for one revision.
2. ~~**`board.h` carries the same stale value.**~~ **Also retracted.** The
   comment on `BOARD_LINK_IRQ` — *"Côté clavier ce signal arrive sur GPIO46"* —
   and the strapping analysis that follows it are both correct. A commit that
   struck them through has been reverted.

   *These two entries are kept struck through rather than deleted: this section
   exists to stop people reconciling documents against each other instead of
   against the source, and the fastest way to relearn that lesson is to see it
   fail once. The source here is the netlist, not any `.md`.*
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
7. **The design spec has been brought up to v2.**
   `docs/superpowers/specs/2026-08-07-lien-s3-coffre-design.md` now carries the
   v2 register table, the two defects it closes, and the decisions behind them —
   but its body was written against v1 and is marked as such rather than
   rewritten. Where the two disagree, this document and the code win, as always.
8. **There is still no read-back of the active USB mode.** Section 6.4 says so
   plainly. `0x0D` would have been the natural place, and it stayed reserved
   because this layout was already in your hands when the question came up.
   Worth reopening together before either half is flashed, if you find you need
   it — it is one byte and one version bump today, and a much larger
   conversation afterwards.


## 13. The DMA channel — browsing accounts and reading codes

**Established**, and running on the chest at commit `440d79d` — but **no frame
has crossed it yet**. Its receive is armed and segments are queued; the exchange
itself is unproven. This is the section to exercise first, and the one most
likely to need a correction.

### Why a second channel at all

The shared register file is **64 bytes** on the P4 and section 1 uses all of
them. Browsing accounts and returning codes needs more room than exists, so it
lives on the slave-HD **DMA channels** (`spi_slave_hd_queue_trans`, segment
mode), which have no such limit.

The split is deliberate: **registers hold what must be readable at any instant
and at constant cost** — magic, version, state, pending operation, instance,
active mode, label, CRC. **DMA carries what is asked for.**

### Wire commands

Taken from `components/hal/esp32p4/include/hal/spi_ll.h:81-90`, not from memory.
In one-line mode the wire byte equals the base command.

| command | byte | when |
|---|---|---|
| `RDBUF` | `0x02` | read the shared registers |
| `WRBUF` | `0x01` | write the shared registers |
| `WRDMA` | `0x03` | send the request segment |
| `WR_END` | `0x07` | **after** the WRDMA — closes the write segment |
| `RDDMA` | `0x04` | read the reply segment |
| `INT0` | `0x08` | **after** the RDDMA — closes the read segment |

Master-side references: `essl_spi_wrdma_done()` emits `WR_END`,
`essl_spi_rddma_done()` emits `INT0`.

### The segment number moves before the pending operation clears

**Guaranteed, and you may rely on it.** Within the tick where the press is
consumed, the chest calls `sec_confirm_authorize()`, computes the code, queues
the segment and bumps `0x11` — *then* publishes the block. The pending operation
only falls back to `0` on the **next** tick, because it is read from
`sec_confirm` at the top of the loop.

So `0x11` always moves **before** `0x06`–`0x07` empties, never after.

**What you may therefore infer**: a pending operation that returns to `0`
**without** the segment number having moved means the prompt was refused or
expired — no code was computed, nothing was queued, and your request is dead.
Cancel it rather than waiting.

That inference also holds when the chest *fails* to serve: no valid time, a slot
that is no longer an OATH account, an HMAC failure. In all of those the chest
queues nothing and the operation clears — which is exactly the case your rule
covers.

### The chest keeps a receive queued at all times

**Obligation, not an implementation detail.** Without a queued receive, your
WRDMA is **lost with no error on either side**. The chest arms one at init —
before its task even starts, so the very first request you send after plug-in
cannot fall into a hole — and re-arms immediately after consuming each segment.

This bit us on the first boot of v3, and the failure mode is exactly as
described: the driver refused the queue because the buffers were not aligned to
a **64-byte cache line** in *address and length*, and the only trace was one log
line. Fixed in `440d79d`, and now three `_Static_assert`s refuse the build rather
than warn at runtime.

**What that costs you: nothing.** The chest rounds the *queued* length up to a
cache line, but **the length published at `0x12`–`0x13` is the real one**. Read
exactly what is announced and ignore the padding — reading the rounded length
would give you bytes that mean nothing and break the reply's CRC.

### The doorbell, and the order that makes it safe

1. You write the request with **WRDMA**, close it with **WR_END**.
2. You **then** increment `0x3C`.
3. The chest sees the doorbell change, consumes the segment, serves it.
4. The chest queues its reply, **then** bumps `0x11`.
5. You see `0x11` change, read `0x12`–`0x13` for the length, issue **RDDMA**,
   close with **INT0**.

Two rules fall out of that order, and both matter:

- **The chest never reads a segment you have not announced.** The doorbell is
  the announcement; a WRDMA without it is ignored.
- **You must never read a segment the chest has not queued.** Wait for `0x11` to
  *change*. Do not trigger on `0x10` being non-zero — it stays non-zero after a
  reply has been read.

On first contact the chest takes your doorbell value as a reference **without
serving anything**. Otherwise a chest reboot would make it serve a request you
consider long gone.

### The request — 8 bytes, fixed

```
0x00      command   (0x01 LIST, 0x02 CODE)
0x01      argument  (LIST: first index; CODE: account index)
0x02-0x05 reserved, zero
0x06-0x07 CRC16 over 0x00..0x05
```

Fixed size, multiple of four — the driver truncates a receive that is not. The
CRC is not decorative: the DMA channel has **no error detection of its own**, and
a corrupted command would arm a confirmation for an account nobody asked for.

A request that fails its CRC, or carries an unattributed command, is **not
served and arms nothing**. You will see the segment number stay put, and retry —
the same signal as a refused confirmation.

### `LIST` — names only, no press, no time

```
0x00  TOTAL number of accounts     <- for your "3/12"
0x01  number in THIS page
0x02  first index of this page
0x03  flags (bit 0 = more follows)
then, per account: index (1), length (1), name (n)
then CRC16 over everything preceding
```

Bounded at **512 bytes**. `LIST` requires **no press and no time**: it returns
only names, which a YKOATH `LIST` already gives the host — so nothing new is
exposed and the owner can scroll freely.

**A page shrinks rather than lies.** The chest tries the whole page and drops
accounts until it fits, because announcing “no more” on a truncated page would
lose accounts in silence. An empty list is still published: you must be able to
tell “nothing to show” from “my request got lost”.

### `CODE` — never without a press

```
0x00      account index
0x01      digit count (6 or 8)
0x02-0x09 code in ASCII, LEFT-padded with zeros
0x0A      seconds left in the window
0x0B      reserved
0x0C-0x0D CRC16 over 0x00..0x0B
```

**The request does not produce a code.** It **arms** a confirmation: the chest
publishes `pending_op`, the instance, and the **label** of the account, then
returns. Nothing is computed and nothing is queued.

You show the label, the owner presses, you write `0x5A` + the instance at
`0x38`–`0x39` — the v2 confirmation protocol, **unchanged**. Only then does the
chest compute the HMAC, queue the segment and bump `0x11`.

**Without that press, step 5 never happens.** The segment is consumed once and
cleared.

Two properties you can rely on:

- **A `CODE` request while a CCID confirmation is already in flight is not
  stolen.** The chest refuses to arm over it; you see the segment number stay
  put and retry. The link has its own confirmation slot, distinct from the CCID
  worker's, so an authorisation meant for the host cannot make a code come out
  on the link.
- **Without a valid time, no code.** Not a code computed on a guessed time,
  which would be wrong while looking right. `TIME_VALID` tells you in advance.

The chest applies the **modulo** on this path — unlike the YKOATH path, where
`ykman` does it. Left-padded with zeros, because a TOTP code is a fixed-length
string: `0418` is not `418`, and a service expecting six digits rejects five.

### What your side owes, and the chest cannot enforce

Your team stated these; they belong in the contract because the chest has no way
to hold them for you:

- a code is **never** displayed without a preceding press;
- it disappears at the end of its window, or as soon as the user navigates;
- it is **never** auto-refreshed: a new code takes a new press;
- the list of names scrolls freely.

The reason, in one sentence: today, five seconds in front of an unattended
keyboard yield **nothing**; if browsing revealed codes, a glance would yield
**all of them**.

### The index may shift between `LIST` and `CODE`, and that is safe

The host can add or delete an account between your list read and your code
request, so the index you send may no longer name the same account.

**What makes that harmless is the label, not the index.** What the owner reads
before pressing is the name the **chest** published at `0x14` when it armed, never
your cached copy. If the index has shifted, she sees the name actually targeted
and does not press.

**Always display the label from the register during the prompt**, never the name
from your own `LIST`.

## 14. Time, and why the chest holds it

**Established.** A TOTP is `HMAC(secret, floor(unix / 30))`: without a time,
there is no code. **The chest has no clock** — no RTC, no backup cell — and your
half has no trustworthy one either (internal RC oscillator, minutes of drift per
day in sleep).

What resolves it is your own observation: **the chest only exists while USB is
plugged, and while USB is plugged your half never sleeps.** It counts on its
40 MHz crystal, under 2 s of drift per day — far inside a 30 s window.

So: **the host sets the time once per plug-in**, over the CCID channel the client
already speaks (`niphar-oath set-time`, extension `INS 0x10`, outside the YKOATH
set). The chest carries it forward on its monotonic clock.

**The invalidation is free, and that is why the time lives in the chest rather
than in your half**: the chest reboots when unplugged, so its time erases itself.
There is no flag to maintain and no stale time possible. Your half survives
unplugging on battery, which is precisely what would have made it the wrong
holder.

**Bit 3 of `0x05`, `TIME_VALID`, never rises on a default.** Not zero, not a
build epoch, not “probably after 2020”. `sec_time_set()` refuses anything below a
plausibility floor (2024-01-01) and **keeps nothing** in that case. A guessed
time would produce wrong codes *presented as right* — worse than no code, because
with “NO TIME” on screen the owner knows what to do.

**A refusal does not clear a time already set.** Otherwise a malicious host would
switch your display off by sending one absurd frame. It can propose a wrong time
— the limit below — but it cannot remove the one that works.

**The limit, stated rather than hidden**: a host that lies about the time makes
the chest compute codes for another window. It learns nothing from that — the
codes only appear on your screen and never return to the host — and at worst a
wrong code is displayed and the service rejects it. This is inherent to the
device class: a lying host is indistinguishable from a week unplugged.

The time survives USB mode switches (the chest does not reboot), so it lives
outside the OATH applet. It is set **in OATH mode**, since CCID carries it.
