# Porting the security stack — OpenPGP CCID and CR-HMAC OTP-HID

Design validated on 2026-08-07. Third increment of the chest firmware.

## 1. Problem

The chest is to become an OpenPGP token and a security key. That stack already
exists, written and proven in `KeSp_firmware`: ~2500 lines, of which the
OpenPGP CCID part has been validated on hardware with gpg 2.4.9. Rewriting it
would be a gratuitous security regression.

Two things currently prevent going all the way, and this spec treats them as
constraints rather than ignoring them:

- **Physical confirmation has no source.** All the strength of the KeSp model
  rests on `sec_confirm`: a real press on a key, which host malware cannot
  fabricate. On the chest, that press will arrive over the SPI link — deferred
  to the revised board. Without a substitute, nothing is executable.
- **The only hardware that exists is the dev kit**, and its firmware must never
  serve as the basis for the chest's by accident.

### Correcting a confusion that nearly changed the scope

FIDO/CTAP **does not exist** in `KeSp_firmware`. Its “security key” is a
YubiKey-compatible CR-HMAC over HID, and FIDO2 is explicitly noted there as
“Phase 3 — out of scope”. What gets ported is therefore **CCID + OTP-HID**; a
real CTAP2 would be new development, to be decided separately.

## 2. One function at a time, commanded by the keyboard

**The chest never exposes more than one USB function.** That is the project's
motto — “plenty of things, one at a time” — and it is a security decision as
much as an ergonomic one: a PGP key must not be present on the bus because
someone wanted a disk.

Four modes, mutually exclusive:

| mode | what the host sees |
|---|---|
| `NONE` | nothing — the state at startup |
| `STORAGE` | the disk (MSC) |
| `PGP` | the OpenPGP card (CCID) |
| `OTP` | the CR-HMAC key (HID) |

**The selector is the S3.** The chest lives in the keyboard's left half:
wherever you take the keyboard, the S3 is there and powered, hence able to
command. On the dev kit, in the absence of an S3, a console command plays that
role — locked by construction like the confirmation crutch (§5).

**At startup, nothing.** A function is only exposed on explicit order, and the
order does not survive unplugging. A chest plugged into an unknown machine
therefore presents no surface until you have asked for something.

Two consequences, one of them awkward:

- **Changing mode forces a re-enumeration.** The interfaces of a configured USB
  device cannot be modified: you must leave the bus and come back with
  different descriptors. The host will see an unplug followed by a replug on
  every switch. That is imposed by USB, not chosen.
- **On `niphar_chest`, as long as the SPI link does not exist, nothing can be
  selected** — the chest firmware will be inert on the USB side. That is
  consistent with the rest: refuse rather than grant by default.

The P4's endpoint budget (16 against the S3's 4) stops being a design argument,
since we never expose more than one set at a time. Comfortable headroom
remains, nothing more.

## 3. Scope

In scope: porting both parts, the USB mode switch and its selector, a
confirmation source usable on the kit, and porting the existing host tests.

Out of scope: the link's SPI transport (its own spec, waiting on the board), a
real CTAP2, and the decision on key storage (§8).

## 4. What gets copied, and what does not

The stack's coupling to `KeSp_firmware` is tiny: it borrows only a
`STORAGE_NAMESPACE "storage"` from `keyboard_config.h`, and `nvs_utils` declares
itself reusable without project dependencies.

```
main/sys/
├── nvs_utils.{c,h}       verbatim from KeSp
└── cr_crc16.{c,h}        moved out of main/link/

main/security/
├── apdu.{c,h}            \
├── ccid.{c,h}             |
├── openpgp_card.{c,h}     |
├── openpgp_crypto.{c,h}   |  copied, near-verbatim
├── openpgp_do.{c,h}       |
├── otp_hid.{c,h}          |
├── otp_proto.{c,h}        |
├── cr_hmac.{c,h}          |
├── sec_confirm.{c,h}     /
├── sec_store.{c,h}       /
├── sec_config.h          NEW — replaces keyboard_config.h
└── sec_gate.{c,h}        NEW — confirmation source
```

**`cr_crc16` moves up into `main/sys/`.** It had been placed in `main/link/` on
2026-08-07 without knowing that the CCID stack would use it too. Two divergent
copies across the two parts would be exactly the silent bug that its own
comment claims to avoid.

**Porting rule:** change as little as possible. A copied file that diverges
from its original loses the benefit of having been proven, and KeSp's future
fixes will no longer carry over to it. Every deliberate divergence is
documented at the top of the file.

## 5. `sec_gate` — the only genuinely new piece

`sec_confirm` stays unchanged: it arms, it expires, it consumes. What is
missing is the source of the press, and that depends on the board.

| board | source | status |
|---|---|---|
| `niphar_chest` | the SPI link | not yet written — operations expire |
| `jc_devkit` | `sec confirm` console command | usable immediately |

**The crutch must not be able to reach production**, and an automatic
confirmation is, in use, indistinguishable from a device that works. It is
therefore locked by construction, on the model of the Secure Boot guard:

- the console variant is only compiled if `BOARD_LINK_AVAILABLE == 0`;
- an `#error` forbids enabling it on a board that has the link;
- `scripts/fast.sh` fails if the console variant appears in a `niphar_chest`
  build.

On the chest, before the link exists, operations will expire after 15 s. That
is the honest behaviour: refuse rather than grant.

## 6. Single-mode USB

Each mode has its own set of descriptors, and only one is mounted at a time.
The endpoints are therefore never in competition: each mode starts again from
the same pair.

| mode | interface | OUT | IN |
|---|---|---|---|
| `STORAGE` | MSC | `0x01` | `0x81` |
| `PGP` | CCID | `0x01` | `0x81` |
| `OTP` | HID | `0x01` | `0x81` |
| `NONE` | — | — | — |

A new module, **`usb_mode`**, owns the USB stack: it installs TinyUSB with the
requested mode's descriptors, and switches by detaching then reinstalling.
`usb_device.c` ceases to be the device's owner and becomes the mechanism that
`usb_mode` drives.

`ccid.c` registers with TinyUSB through `usbd_app_driver_get_cb`, a mechanism
of **raw** TinyUSB — hence compatible with the architecture adopted at the
foundation, where `esp_tinyusb` was set aside. This was not planned: the
wrapper would have been a problem here too.

**Delicate point.** `usbd_app_driver_get_cb` is resolved at link time, not at
run time: the CCID class is present in the binary whatever the mode. What
changes from one mode to another is the **descriptors** — hence what the host
sees. A class that is compiled but not described is never reached, since no
interface is associated with it. This is correct, but it deserves to be known:
the OpenPGP card code is there even in `STORAGE` mode.

## 7. Tests

The port brings in the largest amount of pure logic in the project to date:
`apdu`, `otp_proto`, `cr_hmac`, `cr_crc16`, `sec_confirm`. **KeSp already has
host tests for each of them** — they are ported along with the code, without
rewriting them.

APDU parsing is the new attack surface: arbitrary bytes coming from the host,
interpreted as commands. It must be covered before being wired up.

On the kit, end to end: `gpg --card-status`, key generation, signing a commit,
SSH authentication — with `sec confirm` standing in for a key press. And
KeePassXC for the OTP, which requires the VID patch documented at KeSp.

## 8. Reservations, explicitly not resolved here

**The OTP-HID has never run on hardware**, not even at KeSp — their own docs
say so. The kit will make it possible to validate it for the first time, which
the S3 could not do for lack of endpoints. Until that is done, it remains new
code disguised as proven code.

**Key storage is not settled.** The 2026-08-07 audit showed that the host can
force download mode through the CDC control lines (`docs/HARDWARE.md`), hence
dump the flash and the NVS. The hypothesis inherited from KeSp — plaintext keys
acceptable because extraction requires physical access — no longer holds as-is.

On the kit, no stakes: disposable keys on a development board. But **this port
must not decide by default** what we will do on the chest. The decision (open
JP1/JP2, burn the eFuses, or accept the risk) is taken before the chest carries
a real key, not at the moment the code already exists.

**The kit stays a kit**: no Secure Boot or Flash Encryption option in the
shared defaults. The guardrail already exists in `scripts/fast.sh`.

## 9. Decisions and rejected alternatives

| Decision | Rejected alternative | Reason |
|---|---|---|
| Four exclusive modes, only one mounted | permanent MSC + CCID + HID composite | “one at a time” is the project's motto and a security decision: a PGP key must not be on the bus because someone wanted a disk |
| No function at startup | last mode remembered in NVS | a chest plugged into an unknown machine presents no surface until something has been asked for |
| CCID **and** OTP-HID ported | CCID alone first | they are two distinct modes, and the OTP has never been validatable elsewhere for lack of endpoints |
| Confirmation via the console, kit only | automatic confirmation under Kconfig | nothing would prevent leaving it enabled; a device that confirms itself behaves like a device that works |
| Near-verbatim copy | rewrite adapted to the chest | the code is proven on hardware; diverging costs the benefit and cuts off future fixes |
| `cr_crc16` in `main/sys/` | one copy per part | two divergent CRCs would be a silent bug at the interface, which is precisely what its comment claims to avoid |
