# Windowed updates on the CrowPanel 5.79" (dual SSD1683)

Measured on glass, 2026-08-14, the scrub bench, HELLO run. Everything here
was confirmed by eye or by webcam capture, not reasoned from the datasheet.

---

## The one-paragraph version

Windowing the **NEW** plane is free money: a 6 px bar runs at **108 fps** against
**47.7 fps** for the full-plane path, with a clean image. Windowing the **OLD**
plane is where every artefact in this session came from. The fast burst writer
cannot do it correctly at all; the slow per-byte writer can, at ~2.7× the cost
per byte. So: window NEW freely, and pay for OLD only when you actually need it.

---

## Geometry, because everything depends on it

- Logical screen is 272 × 792, held long-edge vertical.
- The plane is **column-major**: `FB_IDX(r, c) = c * 272 + r`, where `r` is
  logical x (0..271) and `c` is a byte column (0..99).
- One byte holds 8 pixels along **physical X**, and physical X maps to logical y
  as `X = 799 - Y`, with an 8 px seam at logical y 396 that is not visible.
- Therefore **a byte column is a horizontal 8 px strip of logical y, spanning the
  full 272 px width.** A window is a range of byte columns over full rows.
- The die boundary sits at logical y 396: y 0..395 is the **slave** (columns
  50..99), y 396..791 is the **master** (columns 0..49).

Consequences:

1. Byte columns are the **only** cheap axis. A run of byte columns over full rows
   is one contiguous memory block that goes straight to the DMA engine. Banding
   the other way — a narrow strip of logical x — is strided and needs the
   per-byte writer.
2. A window is quantised to 8 px of logical y. Nothing addressed finer than that
   exists, and that quantisation causes real artefacts (see *hold band*).

---

## What works

`EPD_WriteWindowBurst(FB, NULL, c0, c1, /*also_old=*/false, ...)`

NEW plane only, DMA path, ~0.24 µs/byte. 954 bytes/frame for a 6 px bar,
0.84 ms SPI against 8.12 ms panel busy. **108 fps at mode 1.** No artefacts.

## What does not work

**`EPD_WriteWindowBurst` with `also_old = true`.** It writes NEW, then writes OLD
without re-programming the window. The address counter is left past the end of
the NEW burst, so OLD lands on different pixels than NEW. The displaced
differences stay in chip RAM and re-drive on every later update. Symptoms:
progressive darkening, evenly spaced dark lines, and a clear bar landing on the
wrong half of the panel — 396 px away, i.e. mirrored about the die boundary.

Things tried against that, all of which failed:

| attempt | result |
|---|---|
| Re-arm the window before the OLD plane inside the burst writer | Striped **both** dies completely. `write_chip_window` must do this; the burst path must not. |
| `(min,max)` bounds form for the slave's `0xC4` range (`winbbnd 1`) | Slave filled solid, dies split at y=396. Scan-order `(start,end)` is correct — same verdict the per-byte writer had already reached. |
| Write OLD = NEW back over the window after the update ("reconcile") | Did not fix it, and doubled the SPI. |
| Full-plane path for everything | Correct, but 44–48 fps. |

**What actually works for OLD:** `EPD_WriteWindowMasked` (`write_chip_window`),
which re-issues the window registers in full before each plane. ~0.65 µs/byte
instead of 0.24, so a window costs ~2.7× more — but it is correct.

---

## The working test: an alternating painting bar

A 6 px bar sweeps down the panel and **does not erase behind itself**. One sweep
fills the panel black, the next fills it white, for ever. Every pixel therefore
gets a complete black→white round trip, which is the only thing this glass
respects (`EINK_RULES` #2: to-white on already-white *darkens*).

Three details that turned out to matter:

**Hold band, counted in pixels.** The bar is 6 px tall stepping 6 px, so it
covers any given row for exactly one frame — one drive at mode 1 does not reach
black. `hellohold 18` keeps driving the 18 rows behind the leading edge, giving
every row 4 drives. It must be counted in **rows**, not "whatever the window
covers": the window is quantised to 8 px while the bar steps 6, so a
window-shaped hold gave some rows 4 drives and some 5, and the extra one showed
as a line at the trailing edge once it settled.

**Lap wander.** The sweep start walks 1 px per lap, 0..6 then back to 0
(`hellowander`). A fixed start means the same handful of rows take every
transition edge for the whole session, and that wears a line into the glass.
Stepping 3 px at a time was worse than 1 px — it just spaced the lines further
apart.

**White sweep biased 3 px below black.** Otherwise both colours begin on the same
row and both transition edges land on the same pixels.

**Rim.** Loaded white once by the clear, never written again. Note the hold mask
will happily re-drive it every frame if you let it — a white rim reads as "hold
this white" during the white sweep — so the rim is explicitly cleared out of the
mask. That is a per-frame border redraw otherwise, which the rules forbid.

---

## Four findings that cost hours each. Do not re-derive these.

**1. The reconcile pass IS required.** After the update, chip RAM still holds
OLD != NEW inside the window. When the window moves on, that difference stays
there and keeps re-driving. It shows as noise on the trailing edge. A second pass
putting OLD back equal to NEW over the same window fixes it, and it costs ~16 fps
(88.5 → 72.8). I turned it off once, reasoning the mask was confined to the
window so stale differences could not leak — **that reasoning is wrong** and the
glass says so. `hellorecon 1`.

The reconcile currently rewrites NEW *and* OLD when only OLD needs restoring.
`write_chip_window` has no OLD-only mode; adding one would halve that pass.

**2. Black and white need different drive counts.** At a shared 18 px hold
(4 drives at step 6) the white was exactly right and the black came out a shade
too dark. Black completes in fewer drives than white on this glass. Hence
`hellohold` (white) and `helloholdb` (black) as separate knobs.

**3. The cascade seam is physical X 396..403 and nothing else.** Logical y 395
maps to X 404 and y 396 maps to X 395, so the gap between them is X 396..403 —
8 px that no logical y addresses. It is visible on this glass and stays at
whatever the last full-screen fill left there while the sweep paints around it.
It must be painted explicitly.

Two mistakes made while fixing it, both visible from across the room:
- **Do not include X 395 or X 404 in the seam range.** Those are real rows. Every
  frame of seam-hold sets their mask bits too, and 30 frames of extra drive on
  them is a black gradient on the master side of the join.
- **Do not build the seam write's mask from `FB ^ PREV`.** PREV is only refreshed
  over the main window, so in the seam columns it is stale from whenever they
  were last inside it — after a colour sweep that flags an entire 32 px band as
  changed, and driving that band every hold frame is a massive black ghost. The
  mask there must be the seam bits and nothing else.

**4. A hold that is too long makes things worse, not better.** Over-driving past
completion darkens white (`EINK_RULES` #2: to-white on already-white darkens) and
pushes black past the surrounding level. The seam went full white on the black
sweep at hold 40; hold 3 is right. More drive is not more colour.

## Frame rate ledger (mode 1, 15 V, 6 px bar, step 6)

| configuration | fps | SPI | bytes/frame |
|---|---|---|---|
| Full plane, both planes | 47.7 | 12.52 ms | 54,400 |
| Full-plane hold every frame (`helloall` full panel) | 44.2 | 14.23 ms | 54,400 |
| Windowed hold, whole window | 80.5 | 3.95 ms | 3,776 |
| Windowed hold, fixed 18 px band | 69.6 | 5.43 ms | 6,400 |
| Windowed NEW only, no hold | 108.4 | 0.84 ms | 954 |

Panel busy is **8.12 ms at mode 1 and does not vary with content** — two pixels
and 216 k both cost one waveform. So every number above is SPI, and the ceiling
with zero SPI would be ~123 fps.

Mode 2 doubles busy to 16.23 ms → 57.7 fps, with SPI unchanged. Useful control:
if black lands better at mode 2 than at a slow windowed config with the same
frame rate, it is real drive doing it and not the frame simply being long.

---

## Knobs (all live over serial, no reflash)

```
hellobar N      bar height in px, 0 = use the circle/text sprite
hellostep N     px of travel per frame
hellohold N     rows behind the bar that keep being driven, in px
hellowander N   px the sweep start walks per lap, 1 px steps
helloall 0|1    hold the swept colour inside the window
hellorecon 0|1  second pass putting OLD back to NEW (default off)
hellotrail N    drive against the plane N frames back
win 0|1         windowed vs full plane
mode N          custom-LUT drive frames
```

`trail` and `chase` steer the OLD `bar_push()` path used by the scroller /
stripe / dither / checker runs. **The HELLO run does not read them** —
`hellotrail` is its equivalent. This confused things for a while.

---

## Process notes

The board boots **paused** on purpose. Set every knob first, then `hello 1`,
which runs a 5-cycle GC clear and starts. Setting knobs on a running test
measures the old configuration on glass already dirtied by it.

Never change drive voltage without asking, and never change it live under a
running update — a `rails` write reloads the LUT and voltage registers
mid-update and blacked out half the panel when tried.

`balance 1` (charge-balanced WW/BB LUT blocks) is **not** free despite costing no
frames and no SPI: it puts a pulse pair on every static pixel, so at ~95 fps it
drives all 216 k pixels every frame. The panel went near-black. Leave it off.
