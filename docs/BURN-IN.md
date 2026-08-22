# Burn-in prevention

E-paper does not "burn in" the way a plasma or OLED does. The film is not
wearing a picture into phosphor. What you get after a long run is **DC
imbalance and unfinished transitions**: a stain that a short differential
update cannot lift, because as far as the controller is concerned those pixels
have not changed.

A vendor full-refresh (`white`, `scrub`, Ink Flow **Park** / **White out**)
can usually clear it. Prevention is cheaper than recovery, and some stains
need more than one scrub.

---

## NEVER ±18 V. Never anything above ±17 V.

This is the one that is **fast and permanent**. It is not the DC stain the
rest of this note is about, and a scrub will not lift it.

The datasheet maximum on these source rails is **±17 V** (`VSH1 = 0x4B`,
`VSL = 0x3A`). Ink Flow's volts radio stops at ±17. Every port's `volts`
command refuses an out-of-spec value rather than writing it.

**±18 V, or any hack that writes a rail above 17, destroys the film quickly.**
Do not add an 18 V option. Do not poke the register. Do not "just try it for
contrast." There is nothing up there worth the panel.

±15 / ±16 / ±17 are the only legal steps. ±17 is already the boost. Stop.

This is the operator's note. Mechanisms that live elsewhere:

- skipped LUT phase / VCOM — [PITFALLS.md](PITFALLS.md), [panels/SSD1683-crowpanel-5.79.md](panels/SSD1683-crowpanel-5.79.md) §5
- `NEW == OLD` means no drive — [PITFALLS.md](PITFALLS.md)
- letterbox bars on the wire — [STREAMING.md](STREAMING.md)

---

## What actually marks the glass

Every streamed frame fires DISPLAY over the **whole panel**, even when the
payload is an empty delta. Pixels the controller classifies as unchanged get a
skipped LUT phase: source parked at VSS against a negative common. That is a
small DC field, on every update, on every idle pixel.

VCOM `0x08` (−0.2 V) is the fix that stopped whole-panel darkening on a short
run. It is not a licence to leave a static field under a live stream for an
hour. Confirmed on the 1.54": **~1 hour of Bad Apple, Fit (bars), solid
white letterbox, left visible burn-in** in the bar regions. The moving picture
in the middle was fine. The bars never changed, so they took the skipped phase
every frame and nowhere else.

Black bars are the same physics. White showed it first because grey-on-white
is obvious; black shows up as a stained field.

A pixel that went black-to-white and then sat still can also park **grey** —
not age, a stalled transition. A later differential frame will not finish it.

White only holds after a complete black-to-white round trip. Driving white
onto already-white **darkens**. That is why `ww` (white top-up) was tried and
rejected: it smeared the field black. Do not "maintain" white by re-driving
it with the fast table.

---

## Ink Flow knobs, and how they push this

### Drive / mode

Fewer frames = faster, and more likely to abandon a transition. Graphics at
drive 2–3 look sharp because edges are solid and keep moving. Dithered or
photographic content at those modes parks mid-grey. That grey is the start of
a stain if the content then sits.

Drive 12 is the strong table used by **Park** and pause **White out /
Black out**. Those are vendor full-refresh flashes, not the stream LUT. Use
them to clear; do not stream at 12 and call it prevention.

### BB redrive (the dangerous one on stills)

BB = "this pixel just went black; give it N extra full drives, then leave it
alone." It is free in waveform time (carved out of the drive phase, no extra
BUSY). It **overrides trail/chase** — one OLD-plane write per frame, so they
cannot combine. That is why Ink Flow greys trail and chase out when BB > 0.

It exists because trail/chase re-drives *everything* in history, including
pixels that already settled, and that pulls the field dark. BB only top-ups
pixels that are black *now*.

**BB 20 on a clip that keeps sweeping the screen is the intended use.** Black
gets a deep set, then the next white of the picture does the other half of the
round trip.

**BB 20 on static or slow content is how you stain the panel.**

- A still image left in **Send** is a live stream. First frame: most of the
  picture "changes" off the white seed, so every black pixel gets 20 extra
  drives — extra DC in the black direction — then sits. Rails stay up.
  DISPLAY keeps kicking. Status still says `streaming N fps`. That fps is
  the loop, not new picture.
- Slow content with little white (dark UI, night scene, a paused HUD): pixels
  go black, eat N extra black drives, then idle for minutes with no
  white-going recovery. High N makes the one-sided push worse.
- A mostly-white scene is the opposite problem and is why `ww` failed. Do not
  invent a white BB.

Rule of thumb: if the picture would look the same for ten seconds, BB should
be **0** and you should not be in the stream loop. **Park** or pause
**White out / Black out** (drive 12 full clear, analog not cycling the fast
table). If you must hold a still, send it once and stop.

Shipped default is BB 20 because the demo content is video. That default is
wrong for a JPEG.

### Trail / chase

Re-drive recently-changed pixels, both polarities, for N frames. Fixes grey
trails behind moving edges. On a static field it is a no-op after the history
ages out — then you are back to idle DC. Does not combine with BB.

### Fit (bars) vs Fill (crop) vs extend

| bars | what the unused region is | long-run |
|------|---------------------------|----------|
| extend (default) | copies of the picture's edge pixels | safe if the edge has motion |
| solid white / black | idle ink, scanned every frame | **burn-in.** reverse-drive 0.25 s/min is a band-aid |
| Fill (crop) | no bars | no bar stain; you lose the picture edges |

Solid bars still pulse to the opposite polarity for a quarter second each
minute so they do not sit skipped. You will see the letterbox blink. That is
the round trip. Prefer **extend** or **crop** for anything longer than a
quick look.

### Gray 2 / 4 / 16 / 64

These are **spatial dither steps on a 1-bit stream**, not native 4-level
grey. Native 4-level writes both RAM planes as bitplanes and flashes a common
reset every frame; that path is a stills command, not Ink Flow.

`4` is a 2×2 cluster. On a 200×200 it reads as four tones. The CrowPanel
gets the same 1-bit 2×2 pattern — it does not grow a grey LUT. Finer dither
(16, 64) means more pixels flipping in "flat" areas, so more of the panel is
"changed" every frame. That is motion as far as BB and trail are concerned.
Coarse dither plus high BB plus a dark still is the worst mix.

### Send on a still, vs Park, vs pause hold

| | rails | DISPLAY | picture |
|--|--|--|--|
| Send (video / GIF / still) | up | every ACK | live loop. stills keep cycling |
| Pause | up | still every ACK unless you White/Black out | source frozen |
| White out / Black out (while paused) | full-clear at drive 12, then hold | stopped | restores on unpause |
| Park white / Park black | full-clear at drive 12, then hold | stopped | session over |
| Stop | stream ends | stopped | whatever was last on the glass |

---

## What to do

**Video that fills the panel and keeps moving:** mode 3, BB 20, gray 4,
extend or crop. That is the shipped default.

**A still, a paused UI, a dark scene that barely moves:** BB 0. Send the
frame, then Stop or Park. Do not leave `streaming N fps` on a JPEG.

**Letterbox:** extend or crop. Not solid white. Not solid black for an hour.

**After a long run, or if the field looks stained:** full black and full
white, over and over, for a long time — hours, not one `white`. That is
thousands of complete round trips, which is what relaxes the film.

- Ink Flow **Park** (8 flashes at drive 12) is the short form.
- Ink Flow source **Burn-in equalizer**: pick 1–60 minutes, Send. Mode 12,
  vendor full sequence (rails up and down), solid black then solid white
  until the timer ends. Estimated drive counts are in the dropdown.
- Serial: `scrub 10`, `blink 20`, `card` / `card inv`. Leave it looping for
  a few hours if the stain is deep.
- A differential poke, a still left in **Send**, or one White out will not
  do this job.

One minute of video is already on the order of a five-year signage/reader
refresh budget. Plan on restore passes as part of using this, not as an
emergency.

**Do not** invert the plane to "rest" white. **Do not** turn `ww` back on.
**Do not** re-drive white with the fast table.

---

## What is measured vs inferred

Measured: skipped-phase DC on idle pixels; VCOM `0x08` vs `0x78`; `ww`
smearing; 1.54" white letterbox, ~1 hour, visible bar stain; BB is free in
BUSY and overrides trail/chase.

Inferred from that, not a second hour-long matrix: black bars, high BB on a
dark still, stills left in the stream loop. The physics is the same. If a
later run disagrees, put the number here and strike the inference.
