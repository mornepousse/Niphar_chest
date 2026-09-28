# FIDO2 / WebAuthn on the key board — design

Add a FIDO2 (CTAP2) and U2F (CTAP1) authenticator to the `Niphar_chest`
firmware, on all three boards.

Neighbour: [`2026-08-16-carte-cle-wt9932-design.md`](2026-08-16-carte-cle-wt9932-design.md)
for the buttons, the LED and the presence gate; the hardware contract is in
[`docs/HARDWARE.md`](../../HARDWARE.md).

## What we are building, and why

Two uses chosen by the owner:

- **Passkeys** — sign-in with no password and no username, which requires CTAP2
  with resident credentials (*discoverable credentials*).
- **Second factor** on sites that only know U2F.

## Out of scope, and why

| rejected | reason |
|---|---|
| **clientPIN** | dropped after clarification: the initial request came from a confusion with TOTP. Consequences below — they are real and accepted. The design does not preclude it: it remains addable later without redesigning anything |
| **TOTP** (6-digit codes) | nothing to do with FIDO, and **blocked by the absence of a clock**: the board has no battery, so it does not know the time at each plug-in. A separate subject, to be scoped after choosing between a coin cell and a time supplied by the host (which weakens the mechanism: a host that lies about the time makes it compute future codes) |
| `hmac-secret` extension | neither local Linux login nor LUKS was retained; that is the only thing that required it |
| “basic” attestation with a certificate | a self-signed certificate is chained to no authority: it would make the key *look like* a commercial key without bringing anything. Attestation **`none`** |
| signature counter | set to **0**, which the specification allows for an authenticator that does not implement it. It exists to detect cloning — but the keys derive from an unreadable eFuse, so the key is not clonable. Keeping it would cost a flash write on *every* authentication |
| credential management (`credentialManagement`) | to be reconsidered once passkeys are in service; without it, erasing goes through `authenticatorReset` |

### What the absence of a PIN costs

To be written down in black and white, because it is the only real trade-off in
this design:

1. **A stolen key is usable by whoever finds it.** The presence button prevents
   any remote use — a network attacker can do nothing — but someone holding the
   key presses it themselves.
2. **Sites requiring `userVerification: required` will refuse the key.**
   Microsoft and some Google flows are among them. Those that ask for
   `preferred`, the most common case, work normally.
3. The authenticator announces `clientPin: false, uv: false` in `getInfo`.

## Architecture

```
       host (browser, libfido2)
                    │  64-byte HID reports
                    ▼
        usb/mode_fido.c            usage page 0xF1D0, TinyUSB glue
                    │
                    ▼
     security/ctaphid.c    ◄── PURE
                    │
                    ├──────────────► security/u2f.c        (CTAP1, via apdu.c)
                    ▼
        security/ctap2.c            getInfo, makeCredential,
                    │               getAssertion, reset
        ┌───────────┼────────────────┐
        ▼           ▼                ▼
   cbor.c      fido_cred.c    openpgp_crypto.c
   ◄── PURE    resident       ES256 — already exists
               store
                    │
                    ▼
        security/sec_confirm.c      the presence gate
```

The split reproduces the proven one from CCID/OpenPGP: transport → pure frames
→ dispatch → crypto.

### What is pure, and why that is not a detail

| module | what it decides | why it must be pure |
|---|---|---|
| `ctaphid.c` | channels, init/cont packets, reassembly, lengths | **it reads bytes supplied by the host**: an attack surface, on the same footing as `apdu.c` |
| `cbor.c` | canonical decoding and encoding | likewise; malformed CBOR is the classic vector against authenticators |
| domain sanitisation | what is shown on the screen | host text rendered to a human for a security decision |

`openpgp_crypto.c:37` already does ECDSA on `MBEDTLS_ECP_DP_SECP256R1`, that is
to say ES256: reused as-is, already validated on hardware.

## The keys: derived, never stored

```
d   = HMAC(K_maître, 0x01 ‖ nonce ‖ rp_id_hash)     credential private key
tag = HMAC(K_maître, 0x02 ‖ nonce ‖ rp_id_hash)     its authenticator

credential_id = nonce(16) ‖ tag(16)                  32 bytes
```

At authentication we recompute `tag` from the presented nonce and the domain
hash; if it does not match, the credential is not ours and we refuse.
**Non-resident credentials therefore cost no storage at all** and their number
is unlimited.

**The `0x01`/`0x02` prefix bytes are a domain separation, and their absence
would be a vulnerability**: without them, the credential's authenticator
*would be* its private key, and publishing it in the credential ID would amount
to publishing the key. This is not a stylistic precaution.

### Where `K_maître` lives

The P4's HMAC peripheral, key in a read-protected eFuse block.

> *The 256-bit HMAC key is stored in an eFuse key block and can be set as
> read-protected, i.e., the key is not accessible from outside the HMAC
> accelerator.*
> — **ESP32-P4 TRM, ch. 28 “HMAC Accelerator”, p. 1526**

**Upstream mode mandatory.** The same page states that the result is only
accessible to software in *upstream*; *downstream* mode has only two hard-wired
uses (re-enabling JTAG, decrypting the RSA signature peripheral's parameters)
and does not serve an arbitrary derivation.

**Honest limit, not to be hidden**: `K_maître` is never extractable and a flash
dump yields nothing — but **the derived private key passes through RAM**. A
*software* flaw (a bug in the CBOR parser, typically) could exfiltrate it. That
is not a design defect but the limit of the silicon: no mode of the P4 allows
signing without the scalar existing in the clear. It is the direct
justification for `ctaphid.c` and `cbor.c` being pure and tested.

The P4 offers six key blocks (`EFUSE_BLK_KEY0..KEY5`). **Two distinct blocks** —
one for FIDO derivation, one for NVS encryption. No sharing: reusing a key for
two purposes is precisely what the eFuse's *key purpose* field exists to
prevent.

**Burning an eFuse is irreversible.** Hence a fifth board axis,
`BOARD_FIDO_KEY_SOURCE`, alongside the four existing ones:

| board | FIDO mode | `K_maître` | presence |
|---|---|---|---|
| `wt9932_key` | yes | read-protected eFuse, HMAC peripheral | CONFIRM button |
| `jc_devkit` | yes | **plaintext NVS** — *“the devkit stays a devkit”* | console (`sec confirm`) |
| `niphar_chest` | yes | read-protected eFuse | S3 link |

All three boards carry FIDO — unlike the screen, which only exists on the key
board. The devkit needs it for development, and the chest is the project's
final target. But **the devkit never burns an eFuse**: its `K_maître` lives in
plaintext NVS, which has no security value and is perfectly accepted — it is a
development kit, its FIDO keys are disposable.

A consequence not to be missed: a credential created on the devkit **will never
work** on the key board, and vice versa. The keys derive from `K_maître`; two
boards, two masters, two universes of credentials. That is the intended
behaviour, not a defect to fix.

## Storage of resident credentials

One record per resident credential, solely in order to be able to **enumerate**
them — it is enumeration that makes passwordless work. About 260 bytes: domain
hash, credential ID, *user handle*, domain and user name (for the screen),
flags.

- `fido` partition, 64 KB, at offset `0x620000`. `partitions.csv` explicitly
  declares that everything beyond is free and that adding a partition there
  **moves no existing offset**: no flash erasure.
- Ceiling of **64 credentials** (a Yubikey 5 holds 25 to 100).
- **In NVS and not in raw flash**: `nvs_utils.c` exists, and we inherit atomic
  writes and wear levelling without writing a driver.
- **NVS encrypted by the HMAC scheme** (`NVS_SEC_KEY_PROTECT_USING_HMAC`): an
  eFuse block holds the key from which the encryption keys derive, **without
  requiring flash encryption**. This protects the *metadata* — which sites,
  which accounts — which would otherwise be in the clear in a dump. It is a
  privacy property distinct from key protection, and it counts all the more
  given there is no PIN: the list of your accounts must not be obtainable by
  reading the flash.

## The screen

On `getAssertion`, the screen shows **the domain being authenticated to**, in
double-height font. A real anti-phishing property: the browser has verified
that the origin matches the domain, and showing it on a screen that the web
page does not control makes an anomaly noticeable.

Without a PIN, this screen carries more weight: it is **the only thing** that
tells the user what they are authorising before pressing.

**This text comes from the host**, so it is sanitised before being rendered:
printable ASCII only, **explicit and visible** truncation. Rendering a host
string as-is on a screen used for a security decision would be a fault. Pure
logic, hence tested.

The fifth mode adds a fifth cycle dot. `screen_mode_count()` returns 4
hard-coded and `test_mode_count_is_four` pins it as a **layout decision** and
not as the enumeration: that test must go red, that is its job.

## Prerequisites, before the first line of FIDO

1. **`sec_confirm`, Ruling 28.** `authorize()` does not check that the press is
   later than the current arming: a late press can authorise the *next*
   operation. Theoretical on OpenPGP; real with a browser chaining
   `getAssertion` calls. Wiring FIDO onto a faulty gate and discovering it in
   phase 5 would cost far more. **And without a PIN, that gate is the only
   defence left** — all the more reason to repair it first.
2. **Close out `ecran-oled`.** The Critic from its final review: the hardware
   validation of the screen and its recording in `docs/HARDWARE.md`, including
   the electrical residue already measured (*“parasitic addresses persist even
   with the pull-ups, the margin is thin”*), which today is in no committed
   file.

## Phases

| # | content | what works at the end |
|---|---|---|
| **0** | the prerequisites above | we stop building on a faulty gate |
| **1** | `ctaphid.c` (pure), `mode_fido.c`, fifth mode | the host sees a FIDO device; `INIT` and `PING` answer |
| **2** | `cbor.c` (pure) + `authenticatorGetInfo` | `fido2-token -I` describes the key |
| **3** | `u2f.c`, HMAC derivation, confirmation screen | **usable**: second factor on GitHub, Google, GitLab |
| **4** | `ctap2.c`: `makeCredential`/`getAssertion`, non-resident | WebAuthn with `userVerification: preferred` |
| **5** | `fido_cred.c`, `fido` partition, encrypted NVS | **passkeys** |

**U2F before CTAP2**, even though CTAP2 is the goal: U2F exercises *the whole*
chain — transport, presence, derivation, signature, screen — with the smallest
command set. If something breaks in phase 3, the cause is in one of five links;
jumping straight to CTAP2 it would also be in the CBOR and the dispatch. And
phase 3 is already a usable product, not a dead step.

### Split into implementation plans

**This spec yields not one plan but two.** Holding them in a single one would
produce a document nobody re-reads and whose last tasks would be written
against code that will have changed.

- **Plan 1 — phases 0 to 3.** Ends on a usable product: a second factor that
  works on GitHub. That is where we learn whether the transport, the eFuse
  derivation and the screen hold up on hardware.
- **Plan 2 — phases 4 and 5.** Written *after* the feedback from plan 1,
  because that feedback will change things.

Only plan 1 is to be written now.

## Tests

The project's TDD norm applies in full: test first, red before green, and
**transient mutation to prove that the test bites**.

- The pure modules go into `test/`, compiled on the host.
- The CBOR parser is tested on **malformed** inputs as much as valid ones:
  lengths lying about the content, deep nesting, unterminated strings, integers
  out of bounds. That is where the defects that matter live.
- Same requirement for `ctaphid.c`: continuation packets out of order, an
  announced length greater than what was received, unknown channel, `INIT` in
  the middle of a transaction.

**`libfido2` arrives via `nix-shell`** (`shell.nix` at the root, written for
that — the machine runs NixOS): phases 1 and 2 have no browser flow to test
them. And it is better than my own test client for a fundamental reason — **a
client I write shares my own misreadings of the specification**. `libfido2` is
an independent implementation: that is what makes it an oracle rather than a
mirror. Firefox and Chromium (both present) cover phases 4 and 5 via
`webauthn.io`.

The `niphar-security-auditor` agent is required on `ctaphid.c` and `cbor.c`:
these are external input handlers, exactly its domain.

## Order of magnitude

About 1000 lines of firmware and as many of tests — dropping the PIN removes
~900 of the 1900 initially estimated. **It is still not an afternoon**, and the
split into phases exists so that each one is deliverable rather than to give
the illusion of progress.

## Risks

| risk | handling |
|---|---|
| bug in the CBOR or CTAP-HID parser | pure logic, tested, mutation, security audit — it is the most exposed position |
| **key lost or stolen** | without a PIN, it is usable by whoever finds it. No technical handling: it is the accepted trade-off. To be reopened if usage changes |
| eFuse burned by mistake | board axis: the devkit never burns one; the burning procedure is manual and documented, not automatic |
| spontaneous mode switches (open defect) | observed twice, always three switches in under two seconds. **Not diagnosed.** An active FIDO mode disappearing in the middle of an authentication would be very visible — to be resolved before phase 3 |
