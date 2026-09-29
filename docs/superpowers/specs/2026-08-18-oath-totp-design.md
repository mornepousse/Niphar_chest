# OATH/TOTP applet — replacing Proton Authenticator

*Design spec, 2026-08-18. Target: `wt9932_key`, applicable to the chest.*

## Why

Mae holds twelve TOTP accounts in Proton Authenticator. The goal is to move
them onto the Niphar key, so that the second factor lives in the same object as
the OpenPGP and FIDO2 keys — an object you unplug.

Proton remains the backup. The migration is **manual**, account by account:
twelve secrets copied by hand, with no import tool to write and no decrypted
file to move around.

## Decisions taken

Six judgement calls, settled with Mae before design. They constrain everything
that follows.

| # | Decision | Direct consequence |
|---|---|---|
| 1 | **Strict YKOATH model** — the time counter comes from the host | The key has no clock and does not want one. It never knows what time it is. `ykman` is required day to day. |

### Amendment 2026-09-29 — answering the objection that blocked TOTP twice

Two earlier specs ruled TOTP out on the same ground, and **decision 1 adopted the
rejected option without ever answering them**. The objection, verbatim from
`2026-08-17-ecran-oled-carte-cle-design.md:249`:

> a malicious host could then lie about the date and have codes produced for any
> future instant, on a device whose whole point is not to trust the host.

**The objection is real, and it is not answered by decision 2.** A press per code
stops bulk harvesting, but a host that lies about the date gets one code valid at
a chosen future instant for each press it obtains — and the screen shows the
account, never the time, so there is nothing to notice.

**It is however inherent to the whole class of device, not to this design.** A
token without a battery-backed clock cannot distinguish a host that lies from a
token that was simply unplugged for a week. YubiKey's OATH applet — the reference
implementation this one follows — has exactly the same exposure, for exactly the
same reason. The alternative was ruled out by hardware, not by preference: see
`docs/HARDWARE.md`, “the chest does not exist on battery”.

**What would bound it, and what would not.** Refusing a counter *older* than the
last one seen is free, safe, and stops replay of past windows; it needs one
persisted counter and no clock. Refusing a counter too far *forward* is not
implementable: a legitimate week unplugged and a host lying by a week are the
same input. Forward-only monotonicity is therefore a real improvement and a
partial one — **not implemented today**, recorded here so the next reader does
not have to rediscover the reasoning.

**Why the decision stands.** The threat it leaves open — one future code per
press physically given — is narrower than the one it closes: twelve TOTP secrets
sitting in a phone application, harvestable in bulk by any process that reads its
storage. The comparison that matters is against where the secrets live today, not
against a token that does not exist.

| 2 | **Mandatory press on every code** | `CALCULATE ALL` cannot return any code: it answers `TAG_TOUCH` for every account. `ykman oath accounts code` with no argument only shows `[Requires Touch]`. |
| 3 | **No password** | No `SET CODE` / `VALIDATE`. The list of accounts and their modification are readable by any software on the host machine; the secrets, never. |
| 4 | **The screen names the requested account** | The name comes from the host: it must be sanitised before being drawn. Without that, the press is a plain presence switch, not an agreement on an account. |
| 5 | **Sixth mode `usb mode oath`** | One applet per mode, consistent with “plenty of things, one at a time”. No second applet grafted onto the PGP mode. |
| 6 | **A single, widened store** (`sec_store`) rather than a dedicated `oath_store` | A single place to look at what the key holds. Zero cost: see “Current state”. |

## Current state — what we build on

Three facts measured in the repo before design, two of which change the plan.

**`cr_hmac_sha1()` exists** (`security/cr_hmac.c`) and it is exactly TOTP's
primitive (RFC 6238 by default). mbedtls is available for SHA-256.

**`dongle_confirm()` exists** (`security/ccid.c:431`): it arms `sec_confirm`,
polls every 20 ms, and **emits a CCID time-extension frame (WTX) every 1.5 s**
so that the host waits instead of giving up. That is precisely what `ykman`
needs — see “Waiting for the press” — and it is already proven on hardware with
`gpg`. No state machine to invent.

**`sec_store` is inert.** `sec_store_set_slot()` has no caller anywhere in the
repo, and `sec_store_init()` is never called either. The store is therefore
neither loaded nor written, ever. Two consequences:

- widening the structure migrates nothing and can lose nothing — decision 6 is
  free;
- **the CR-HMAC OTP mode cannot work today**, since it reads slots that no path
  fills. A pre-existing defect, independent of this work; the OATH applet
  incidentally brings it the provisioning path it was missing.

## Components

| file | role | pure? |
|---|---|---|
| `security/oath_proto.{c,h}` | parsing YKOATH commands, RFC 4226 truncation, serialising responses, `SEND REMAINING` chunking | **yes** |
| `security/oath_name.{c,h}` | sanitising the host-supplied name before the screen | **yes** |
| `security/sec_store.{c,h}` | widened: 16 slots, 64 B name, 64 B secret; NVS blob version 2 | yes |
| `security/ccid.c` | `dongle_confirm()` widened to carry a label | no |
| `usb/mode_oath.{c,h}` | CCID plumbing, on the model of `mode_pgp.c` | no |
| `usb/usb_mode.{c,h}` | `USB_MODE_OATH` in the enumeration and the cycle | no |
| `hmi/screen_view.h` | `SEC_OP_OATH` and its label | yes |

The bulk of the work is in pure logic, hence under TDD and verifiable without
hardware. That is no accident: it is the host harness's constraint that forces
this split, and it happens to suit this case well.

## Protocol surface

AID: `A0 00 00 05 27 21 01`. All the values below are read from
`yubikit/oath.py` of **ykman 5.9.1**, not reconstructed from memory.

### Commands implemented

| INS | command | behaviour |
|---|---|---|
| `A4` P1=04 | SELECT — selecting the applet | returns `TAG_VERSION` + `TAG_NAME` (salt) |
| `01` | PUT | adds an account; touch flag **forced** whatever the host asks for |
| `02` | DELETE | deletes an account |
| `05` | RENAME | renames an account |
| `04` | RESET | erases all OATH accounts |
| `A1` | LIST | lists names and their type |
| `A2` | CALCULATE | **arms the confirmation**, then returns the truncated code |
| `A4` P2=01 | CALCULATE ALL | returns `TAG_TOUCH` for every account, never a code |
| `A5` | SEND REMAINING | continuation of a truncated response |

**Instruction-byte collision, not to be missed.** `SELECT` (ISO 7816) and
`CALCULATE ALL` are **both `A4`**. They differ only by their parameters:
`ykman` sends `CLA=00 INS=A4 P1=04 P2=00` for selection and
`CLA=00 INS=A4 P1=00 P2=01` for the global calculation (`send_apdu(0,
INS_CALCULATE_ALL, 0, 1, …)` in `oath.py`). A dispatch on INS alone would
answer a SELECT response to a request for codes.

### Commands refused

| INS | command | response | why |
|---|---|---|---|
| `03` | SET CODE | `6A81` | decision 3: no password |
| `A3` | VALIDATE | `6A81` | with no password, nothing to validate |
| — | PUT of an HOTP account | `6A81` | see “Scope” |
| — | PUT of a SHA-256 account | `6A81` | see “Scope” — declared divergence |

### Response to SELECT

`ykman` does `data[TAG_VERSION]` with no guard: **the absence of `TAG_VERSION`
(`0x79`) raises an exception on the host side**, it is not optional. And
`_get_device_id(salt)` computes a SHA-256 of the `TAG_NAME` field (`0x71`), so
its absence crashes too. The response therefore carries both:

- `TAG_VERSION`: three bytes. We announce **5.7.1**, a version that selects
  `ykman`'s modern paths (above 3.0.0, it avoids a workaround inherited from
  the YubiKey NEO). It is an announcement of protocol compatibility, not a
  claim to be a YubiKey; to be documented as such.
- `TAG_NAME`: an eight-byte salt, **random, drawn once and persisted in NVS**.
  Stable from one session to the next, otherwise `ykman` would see a different
  device at every plug-in. Drawn at random rather than derived from the MAC
  address: the device identifier published to the host has no business
  revealing a hardware identifier.
- `TAG_CHALLENGE`: **absent**, which signals “no password”.

### Chunking long responses

`LIST` on twelve accounts exceeds the 255 bytes of a short APDU response.
`SEND REMAINING` (`A5`) is therefore **mandatory**, not optional: `ykman` calls
it automatically as long as the status word is `61xx`. An implementation that
omits it works with three accounts and breaks with twelve — the kind of defect
that only shows up after the migration.

## Waiting for the press

`ykman.calculate()` sends the APDU and unpacks the response. **No retry loop,
no touch handling in the library.** The card must therefore hold the command
open until the press — the opposite of U2F, where the client retries every
~117 ms.

```
ykman                          key
  │  SELECT AID A0000005272101 →
  │                            ← TAG_VERSION 5.7.1, TAG_NAME <salt>
  │  CALCULATE (name, 8 B challenge) →
  │                              dongle_confirm(SEC_OP_OATH, "GITHUB")
  │                              screen: CONFIRMER / CODE OTP / GITHUB + bar
  │  ← WTX every 1.5 s ──────────  (ykman waits, no error)
  │                              ┌ press   → HMAC, RFC 4226 truncation
  │                              └ 15 s    → 0x6985
  │  ← TAG_TRUNCATED / 6985 ─────
```

The eight-byte challenge is the time counter computed by `ykman`.

**The fifteen-second deadline is real here**, since `ykman` does not retry —
unlike U2F, whose re-arming on every attempt caused the countdown bar to be
removed on 2026-08-18. `screen_op_has_deadline()` returns `true` by default for
any non-FIDO operation: OATH therefore inherits the bar without an extra line,
and for the right reason.

## Storage

```c
#define SEC_N_SLOTS     16
#define SEC_LABEL_LEN   64
#define SEC_SECRET_MAX  64

typedef struct {
    uint8_t type;        /* 0 vide | 0x01 CR-HMAC | octet d'algo YKOATH */
    uint8_t flags;       /* bit0 = appui requis — forcé à 1 */
    uint8_t secret_len;
    uint8_t digits;      /* 6 ou 8 — remplace `reserved` */
    char    label[SEC_LABEL_LEN];
    uint8_t secret[SEC_SECRET_MAX];
} sec_slot_t;
```

2112 bytes of NVS blob, version **2**. The `type` field directly carries the
YKOATH algorithm byte — high nibble `0x20` for TOTP, low nibble `0x01` SHA-1 /
`0x02` SHA-256 — which can never equal `0x01` on its own: no collision with the
existing CR-HMAC slots. The version 1 blob is refused on its size at load time,
with no loss since nothing was ever written to it.

Sixteen slots for twelve accounts: some headroom, without provisioning for a
need that does not exist.

## Scope

**In**: TOTP **SHA-1**, six or eight digits, period carried by the name
(`30/Issuer:account`, YKOATH convention).

**Out, and explicitly refused**:

- **SHA-256** (`6A81`). **This line said the opposite until 2026-08-19** — the
  section announced “TOTP SHA-1 **and SHA-256**” as in scope, whereas
  `oath_do_put()` has refused anything that is not `OATH_ALGO_TOTP_SHA1`
  (`0x21`) since day one. The choice is upheld: implementing an algorithm that
  **no real account exercises** would be speculative, and an HMAC-SHA-256 that
  nothing tests would return perfectly well-formed and wrong codes, forever,
  with no error to say so. A blunt refusal is better than a silent lie. **We
  will do it if the Proton export contains some** — it is the migration of the
  twelve accounts that will settle it, not a supposition.
  The defect was not the refusal but its lack of declaration: it is now
  recorded in `.tripwire-divergences`, which will turn red if the guard
  silently disappears.
- **HOTP** (`6A81`). It requires a persistent counter incremented on every use:
  a state machine and one NVS write per code produced, for a need Mae does not
  have. If the Proton export contains HOTP, we will see it at migration time.
- **SHA-512** (`6A81`). Nothing uses it among the twelve accounts; adding it
  costs little but can be tested against nothing.
- **Password** — decision 3.

## Sanitising the name

The name comes from the host: up to 64 arbitrary bytes, drawn on a screen that
Mae uses to decide. Three rules, all in pure logic and testable.

1. **Keep the full name, without the period prefix.**
   `30/GitHub:mae@ex.org` → `GITHUB:MAE@EX.ORG`. The `30/` is YKOATH
   convention, not meaning: it says nothing to whoever looks at the screen.
   Everything else is kept.

   > **Amended on 2026-08-19 — the previous version was wrong.** It said:
   > “Keep the issuer. `GitHub:mae@exemple.org` → `GITHUB`. That is the part
   > Mae recognises; **the account matters little when there is only one per
   > service.**” The premise in bold is contradicted by the owner: OVH and
   > Ankama will each have **a personal account and a work account**.
   > `OVH:perso` and `OVH:pro` therefore both returned `OVH` — strictly
   > indistinguishable on the screen that is used to decide. For those
   > accounts, the press went back to being a presence switch, and decision 4
   > no longer protected anything. The pair is in the central test
   > `test_noms_distincts_restent_distincts`, observed red before the fix.
2. **Any character without a glyph becomes `?`, never a blank.** `screen.c`'s
   font defines `A-Z`, `a-z`, `0-9` and `?` — **no punctuation** — and its
   current fallback returns an empty glyph for an undrawn character within the
   range. Without this rule, `GitHub:mae` and `GitHub mae` would display
   **identically**: exactly the attack decision 4 aims to prevent.
3. **Truncate at twenty-one characters with a visible marker.** Without a
   marker, two accounts whose names diverge after the cut would be
   indistinguishable, and the press would go back to being blind. The
   second-to-last drawn character carries a fingerprint of the whole name, for
   the same reason.

   > **Corrected on 2026-08-19 — it was a font error.** The previous version
   > said “truncate at **ten** characters. Ten is the real width in
   > **double-height** font over 128 px”. The measurement was right, but it
   > does not apply to this line: in `main/hmi/screen.c`, the account label is
   > drawn by `draw_text_centered()` — **single**-height font,
   > `SCREEN_CHAR_PX` = 6 px, hence 128 / 6 = **21** characters. Only the
   > operation label (`CODE OTP`, `RESET OATH`) goes through
   > `draw_text_2x_centered()` and is subject to the ten-character constraint.
   > The display budget for the name was therefore halved for no reason, which
   > made truncation far more aggressive than it needed to be — and bore
   > directly on rule 1. `OATH_NAME_DISPLAY_MAX` is now 22 (21 drawn +
   > terminator); a twenty-second character would make 132 px,
   > `screen_center_x()` would clamp to the left and `fb_set_pixel()` would
   > amputate the last glyph — the one that carries the marker. Verified by
   > `test_le_nom_de_compte_tient_en_police_simple`.

Rule 2 fixes a font defect that will also affect the site display for FIDO2
passkeys (task #42): fixing it here fixes it for both.

## Errors

| situation | response |
|---|---|
| applet not selected | `6A82` |
| unknown command | `6D00` |
| PUT beyond sixteen accounts | `6A84` (memory full) |
| unknown name on CALCULATE / DELETE / RENAME | `6A82` |
| challenge of length ≠ 8 | `6A80` |
| press not given within fifteen seconds | `6985` |
| secret longer than 64 bytes | `6A80` |

Every piece of host-supplied data is bounded before use: name length, secret
length, challenge length, number of TLVs. This is the attack surface of this
mode, and it will go through `niphar-security-auditor` before merging.

## Tests

The project's TDD norm: test written first, red observed, green after, and
**mandatory mutation** to prove that it bites.

- `oath_proto` — RFC 4226 truncation on the RFC's vectors; `SEND REMAINING`
  chunking with twelve accounts; refusal of out-of-scope commands; bounds on
  every host-supplied length.
- `oath_name` — issuer extracted, glyph-less characters made visible,
  truncation marked. **Trap to avoid**: comparing a name against a constant
  proves nothing about whether two *different* names stay *distinguishable*.
  The assertions compare pairs of names against each other, the way
  `screen_op_has_deadline()` compares the two families of operations.
- `sec_store` v2 — slot bounds, refusal of excessive lengths, invariant “no
  secret leaves through any public function other than
  `sec_store_get_secret()`”.

Hardware validation, at the end of the run: `ykman oath accounts add` for a
test account, then `ykman oath accounts code <name>` with a real press, code
checked against `oathtool` on the same secret and the same instant.

## Risks

**The only serious protocol risk is cleared**: we read `ykman`'s source rather
than assuming, and the waiting mechanism (WTX) already exists and runs.

Left to watch:

- **The lifetime of the wait.** `dongle_confirm()` blocks the CCID task for up
  to fifteen seconds. The USB teardown path during the wait is already handled
  in `ccid.c` (`s_shutdown`); it must be checked that OATH really takes that
  same path and does not bypass it.
- **The migration of the twelve accounts.** It goes through
  `ykman oath accounts add`, hence through `PUT`: a bounding defect on `PUT`
  would show up during the migration, with the real secrets in hand. The bounds
  tests must be green *before* the first real account.
