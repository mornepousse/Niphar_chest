# OLED screen — UX refinement

Addendum to [`2026-08-17-ecran-oled-carte-cle-design.md`](2026-08-17-ecran-oled-carte-cle-design.md).
Written after Mae saw task 4's screens on the real panel.

## The observation, in her words

> “right now the ux is just text in the top left”
> “for instance 'at rest' there's nothing on the screen just 'at rest' in the corner in tiny letters”
> “when i navigate the screen is 10% used”

This is not a matter of taste: **90 % of a 128×64 panel is empty**, and the
three pieces of information displayed (title, line, bar) are all the same size.
Nothing says what matters.

Precise diagnosis of what `render_frame()` does
(`main/hmi/screen.c:218`):

| symptom | cause in the code |
|---|---|
| everything in the top left | `draw_text(title_x, 4)`, `draw_text(2, 24)` — fixed coordinates, no centring |
| everything the same size | a single font, 5×7 in a 6×8 cell (`s_font`, `screen.c:60`) |
| empty screen at rest | `SCREEN_IDLE` has only a title and no `line` |
| splash version cut off | `draw_text(SCREEN_LOGO_WIDTH + 4, 32, NIPHAR_VERSION)` at x=68: 60 px left = 10 characters, but `git describe` returns `6da0d70-dirty` (13) for lack of a tag |

That last point is **not** a memory defect: `fb_set_pixel()`
(`screen.c:151`) bounds all four sides and the truncation is explicitly wanted
for the slide animation. It is a layout defect.

## The guiding principle

**One single piece of information dominates each screen, and it fills the
panel.** Everything else is context, small, at the edges. That is what
distinguishes a device from a debug console.

## The five changes, in order of effect

### 1. Double-height font for the line that matters

12×16 by **pixel doubling** from the existing 5×7 font — no new data in flash,
about thirty lines. Yields 10 characters per line (`SIGNATURE` = 9,
`DECHIFFRER` = 10, `AUTH` = 4: it fits).

Why doubling rather than a real 12×16 font: an extra glyph table would cost
~2 KiB, and at that size on a 0.96" OLED the coarseness of doubling is barely
perceptible. If it shows to the eye, we will buy the real font later — but we
do not pay in advance.

### 2. Full-width inverted banner

White on black, 128 px wide, 10 px tall. On a monochrome display it is the
element with the most visual weight, and it fills the width — which nothing
does today. Cost: inverting a byte of the buffer.

**It carries the mode, not the state.** `AU REPOS` says nothing; whether the
PGP card is exposed is precisely the information that matters. So:
`RIEN EXPOSE` / `DISQUE` / `CARTE PGP` / `CLE OTP`.

### 3. Four cycle dots

`○ ● ○ ○` — which mode out of the four. This is the **only addition that brings
information Mae does not have**: the LED gives a colour that has to be
memorised (her question “I'm on blue but what does it do?” comes from that),
the dots say where you are **and how many presses to get where you want**.

### 4. Seconds remaining as a number, under the bar

The bar says there is time left, the number says how much. Two generations of
keys were lost to invisible expiries — this is not decorative.

### 5. Check mark and cross drawn for the verdict

Two bitmaps of ~32 bytes in place of `ACCORDE` / `REFUSE` in small type. Read
without reading.

## The four refined screens

```
AT REST                       AWAITING CONFIRMATION
┌─────────────────────┐      ┌─────────────────────┐
│    ▄▄▀▀███▀▀▄▄      │      │▓▓▓▓▓ CONFIRMER ▓▓▓▓▓│
│   ██  ▄▄▄▄▄  ██     │      │                     │
│   ██  ▀▀▀▀▀  ██     │      │  ███ ██ ███ █  ███  │
│    ▀▀▄▄███▄▄▀▀      │      │  ██  ██ ██ ███ ██   │  12×16, centred
│                     │      │  ███ ██ ███ █  ███  │
│▓▓▓▓ RIEN EXPOSE ▓▓▓▓│      │▇▇▇▇▇▇▇▇▇▇▇░░░░░░░░░░│  full width
└─────────────────────┘      │        11 s         │
                             └─────────────────────┘

VERDICT                       MODE SWITCH
┌─────────────────────┐      ┌─────────────────────┐
│                     │      │ MODE                │
│         ▄▄██        │      │                     │
│       ▄███▀         │      │  ███  ███  ███      │
│  ▄██▄███▀           │      │  ██   ██   ██       │  slides
│   ▀███▀             │      │  ███  ███  ███      │
│                     │      │                     │
│▓▓▓▓▓ ACCORDE ▓▓▓▓▓▓▓│      │   ○  ●  ○  ○        │
└─────────────────────┘      └─────────────────────┘
```

The rest screen carries the Niphargus logo (Mae's decision: “for at rest put
the niphar logo instead”), already in flash since task 6 (`screen_logo.h`,
512 B).

## Burn-in, settled

An OLED displaying 1425 lit pixels permanently **burns**: the logo would etch
itself into the panel within a few weeks of use.

Three cumulative measures:
1. The logo **drifts** by ±4 px — `screen_shift_px()` already exists (task 3,
   `screen_anim.h`), it just has to be applied at rest and not only to the
   text.
2. The screen **switches off** after a minute of inactivity (SSD1306 command
   `0xAE`), and comes back on at the first press or the first event.
3. No inverted element is permanent: the banner only appears with the logo,
   which drifts.

Without point 2, point 1 merely spreads the burn over 8 more pixels.

## What is pure and therefore tested first

These functions know nothing of screen geometry and go into `test/`:

- `screen_text_px(const char *s)` — width of a text, for centring
- `screen_center_x(uint16_t width_px, uint16_t text_px)` — centred origin, never negative
- `screen_mode_index(usb_mode_t)` → 0..3, and `screen_mode_count()` — the dots
- `screen_seconds_left(armed_at_ms, now_ms)` — the number, rounded **up**
  (showing “0 s” while 900 ms remain would lie in the dangerous direction)
- `screen_verdict_glyph(led_event_t)` → check / cross / nothing
- `screen_blank_after_ms(last_activity_ms, now_ms)` — the blanking

The drawing (`draw_*`, `render_*`) stays in `screen.c`: those are pixels, not
decisions.

## What this does not do

**No navigable menu.** Mae explicitly rejected that reading (“not refined”). A
menu able to modify the key's state would be a new physical attack surface, and
that is not what is being asked for.
