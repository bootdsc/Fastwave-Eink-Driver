# Driving the SSD1683 fast — CrowPanel 5.79", measured

Reference for the dual-SSD1683 CrowPanel 5.79" (GDEY0579T93 glass, 792x272).
Everything here was measured on glass on 2026-08-07 unless marked otherwise.
Register semantics come from the Solomon Systech **SSD1683 datasheet Rev 1.0**
(sections 6.6–6.7 and the command table).

**Headline: the vendor partial costs 336 ms. The same panel does a readable
update in 89 ms.** Nothing about that is a trick of measurement — it is the same
BUSY pin, timed the same way.

---

## 1. The timing law

```
busy_ms  =  fixed_cost(activate_byte)  +  frames x frame_period
```

Both terms are chosen by the host. Neither had been touched before this work.

There is **no content dependence at all**. Min, median and max BUSY over hundreds
of frames are identical to the millisecond, whatever is on the glass. Anything
that tries to speed up e-paper by being clever about *what* it sends is solving
the wrong problem on this controller.

### frame_period — from LUT byte 224 (FR)

Measured with an all-VSS probe waveform (see §6), against datasheet Table 6-7:

| FR code | nominal | measured us/frame |
|---------|---------|-------------------|
| `0x01` | 25 Hz | 40556 |
| `0x02` | 50 Hz | 20306 |
| `0x03` | 75 Hz | 13509 |
| `0x04` | 100 Hz | 10151 |
| **`0x05`** | **125 Hz** | **8112** |
| `0x09` | 37.5 Hz | 27017 |
| `0x0A` | 62.5 Hz | 16223 |
| `0x0B` | 87.5 Hz | 11607 |
| `0x0C` | 112.5 Hz | 9029 |

**125 Hz (`0x05`) is the fastest legal rate — 8.11 ms per frame.** Every other
value is undefined; the datasheet marks them NA.

> The Good Display partial table circulating for this panel family ships
> `FR = 0x08`, which is **not a defined code**. Any port of that table has been
> running an undefined frame rate. Fix it to `0x05` before drawing conclusions
> from it.

### fixed_cost — from the `0x22` activate byte

Same waveform, same rate, only the activate byte differs:

| activate | 1 frame | 32 frames | us/frame | **fixed** | meaning |
|----------|---------|-----------|----------|-----------|---------|
| `0xCF` | 132 ms | 383 ms | 8110 | **124 ms** | ramp up, display, ramp down |
| `0xDC` | 336 ms | 336 ms | — | — | **ignores the custom LUT** |
| `0xCC` | 99 ms | 351 ms | 8110 | 92 ms | ramp up, display, rails left up |
| `0x8C` | 8 ms | 259 ms | 8110 | **0 ms** | clock + display, rails already up |
| **`0x0C`** | **8 ms** | 259 ms | 8110 | **0 ms** | display only, rails already up |

**Hold the analog rails up and the ~91–124 ms power-sequencing cost disappears
entirely.** That is the single biggest win available on this part, and it is free.

---

## 2. `0x22` is a bitfield

The datasheet prints it as a menu of magic values. It is not — it is bits, and
decoding it explains several long-standing mysteries:

| bit | meaning |
|-----|---------|
| 7 | enable clock |
| 6 | enable analog |
| 5 | load temperature value |
| 4 | load LUT from OTP |
| 3 | colour mode: 0 = 3-colour, 1 = black/white |
| 2 | **DISPLAY** (run the waveform) |
| 1 | disable analog |
| 0 | disable clock |

Consequences, all confirmed on glass:

- **`0xC7` was never a "fast" byte.** It is DISPLAY in *3-colour* mode on a b/w
  panel. That is the inverted output and the left/right cascade disagreement
  people report — not a LUT problem. `0xCF` is the b/w equivalent.
  `bb_epaper`'s stock `EP579` fast path uses `0xC7`.
- **`0xDC` discards a custom LUT.** It returns 336 ms whether the loaded LUT asks
  for 1 frame or 32. Bit 4 is clear, so this is presumably the load-temperature
  bit (5) re-selecting the OTP bank. Either way: measured, not assumed.
- **`0x0C` on the factory OTP waveform hangs the panel.** BUSY never releases;
  you get 8-second timeouts and eventually a board reset. Rails-up bytes are for
  custom LUTs only. Never let a global "fast byte" setting follow the user into
  an OTP mode.

---

## 3. Waveform (LUT) format — 233 bytes

```
bytes 0..226   -> register 0x32   (LUT; 0xB2 for the cascade slave)
byte  227      -> register 0x3F   EOPT
byte  228      -> register 0x03   VGH
bytes 229..231 -> register 0x04   VSH1, VSH2, VSL
byte  232      -> register 0x2C   VCOM
byte  224      -> FR (frame rate), inside the 0x32 payload
bytes 225,226  -> XON
```

Black/white mode has **5 LUTs x 6 groups**:

```
block 0 = LUTC (VCOM)   1 = LUTWW   2 = LUTBW   3 = LUTWB   4 = LUTBB
offset  = block*42 + group*7
```

Each group is 7 bytes, **in this order**:

| byte | field |
|------|-------|
| 0 | `RP[n]` group repeat (0 = group skipped) |
| 1 | phase A: `VS[7:6] | TP[5:0]` |
| 2 | phase B |
| 3 | phase C |
| 4 | phase D |
| 5 | `SR[nAB]` repeat for phases A+B |
| 6 | `SR[nCD]` repeat for phases C+D |

`TP` is the phase length **in frames**, 0–63 (0 skips the phase).
`VS` levels: `00` = VSS / DCVCOM, `01` = VSH1, `10` = VSL, `11` = VSH2 (for the
VCOM LUT, `11` = floating).

> **RP comes first, not last.** This is the one thing that is easy to get wrong
> and it silently produces a dead waveform: under the SSD1680-style ordering
> (`TPA,TPB,SRAB,TPC,TPD,SRCD,RP`) the vendor's own table decodes to `RP = 0`,
> i.e. every group skipped. The order above is the only self-consistent reading
> of the shipped Good Display table, and it matches the datasheet figure.

Frames run by a group:

```
frames = RP * ( SR_AB*(TP_A + TP_B) + SR_CD*(TP_C + TP_D) )
```

Total waveform length is the **maximum** over the five blocks — they all run on
one timeline concurrently. **Adding phases to an otherwise-idle block is free.**

---

## 4. Voltage registers

| reg | field | encoding | POR |
|-----|-------|----------|-----|
| `0x03` | VGH | `0x03`=10 V … `0x17`=20 V, 0.5 V steps | `0x17` |
| `0x04` A | VSH1 | `0x21`=8.6 V … `0x41`=15 V … `0x4B`=17 V, 0.2 V steps | `0x41` |
| `0x04` B | VSH2 | `0x8E`=2.4 V … `0xCC`=8.6 V (low range), or 0x21..0x4B | `0xA8` |
| `0x04` C | VSL | `0x0A`=−5 V … `0x32`=−15 V … `0x3A`=−17 V, 0.5 V steps | `0x32` |
| `0x2C` | VCOM | **`0x08`=−0.2 V … `0x78`=−3 V**, 0.1 V per `0x04` | — |
| `0x3F` | EOPT | `0x22` = 2 discharge scan frames, `0x07` = keep pixel voltage | — |

Two traps:

- **`VCOM = 0x00` is not a valid setting.** The register only defines
  `0x08`..`0x78`; everything else is NA. The Good Display table's trailer byte
  232 is `0x00`, so anything that blindly writes that trailer pushes an undefined
  value. Symptom when we did it: inverted disc and flashing.
- **Never write the voltage trailer as a block.** `EOPT = 0x22` costs **+17 ms on
  every update** (measured: 202→219, 137→154, 105→121) and produces a visible
  flash, because those are two extra discharge *scan frames*. Write the
  registers individually — `EPD_WriteLutVoltagesMaskTarget()` exists for exactly
  this.

---

## 5. The DC-bias problem, and the fix

**Symptom:** with a short custom waveform the entire panel creeps darker with
every update, forming a fuzzy grey background, while anywhere the moving content
has been stays white.

**Cause:** pixels that are *not* changing get a skipped LUT phase, which parks
their source at VSS while VCOM sits at the factory bias (well below zero). That
is a small DC field across every idle pixel, on every update, and it accumulates.

**Fix: write VCOM alone to `0x08` (−0.2 V).** Zero frames, zero milliseconds.

Confirmed by moving it in both directions on the glass:

| VCOM | result |
|------|--------|
| factory | whole panel creeps darker every frame |
| `0x08` (−0.2 V) | **open white stays perfectly white** |
| `0x78` (−3 V) | background goes very dark, fast |

Same waveform and same rails-up activate byte in all three, so the effect is
attributable to VCOM alone. **This also rules out the rails-up trick as the
cause** — holding the rails up is not what darkened the panel, so the 91 ms
saving carries no penalty.

### What did not work

- **Charge-balanced filler on the idle LUTs** (equal +VSH1 / −VSL phases on
  LUTWW and LUTBB, free in frames). Sound in theory, broke the image completely
  in practice. Not pursued further once the VCOM fix landed.

---

## 6. The non-destructive probes

Two measurements that cost nothing and cannot damage the image. Use them first on
any new panel.

**Timing law** (`calib`): build a LUT where all five blocks run `TP` frames at
`VS = 00`. Full time cost, **zero field across the pixel**, so the image cannot
change. Sweep `TP` and read the slope (frame period) and intercept (fixed cost).
Repeat per FR code to prove whether byte 224 is honoured.

**Activate-byte cost** (`seqscan`): same null waveform, vary only the `0x22`
byte. The difference between rows is entirely power sequencing.

---

## 7. Known-good recipe

```
1. Power on, EPD_FastMode1Init_Ex(0xB1)         // fake 100 C -> hottest OTP bank
2. Build LUT: reset 2 / drive N / settle 1, FR = 0x05, VCOM = 0x08
3. wave_load(ws, dual = true, WV_V_VCOM)        // BOTH chips, VCOM only
4. Per frame:
     write NEW plane (0x24 / 0xA4)
     write OLD plane (0x26 / 0xA6) = previous frame   [true differential]
     if rails down: 0x22 = 0xC0, 0x20            // once, not per frame
     0x22 = 0x0C, 0x20                           // DISPLAY, no ramp
     wait BUSY
5. Idle: 0x22 = 0x03, 0x20                       // rails down
```

**Both cascade chips must get the LUT** (`0x32` and `0xB2`). Writing only the
master leaves the two halves on different waveforms — that is what made earlier
"phases only" ports look dark and broken across the seam.

### Measured results, `reset 2 / settle 1`, FR `0x05`

| MODE (= drive frames) | total frames | busy | + SPI | wall clock |
|------|--------------|------|-------|------------|
| 16 | 19 | 154 ms | 86 ms | 240 ms |
| 12 | 15 | 121 ms | 86 ms | 207 ms |
| 10 | 13 | 105 ms | 86 ms | 191 ms |
| **8** | **11** | **89 ms** | 86 ms | **175 ms** |
| 6 | 9 | 73 ms | 86 ms | 159 ms |
| 4 | 7 | 56 ms | 86 ms | 142 ms |
| vendor `0xDC` | — | 336 ms | 43 ms | 379 ms |

---

## 7b. Trail chasing — clearing ghosts for free

**Symptom:** content the moving object has left behind settles at roughly 50%
grey and stays there forever, at every speed. It is not a slow fade.

**Cause:** a B→W transition gets `drive` frames, lands short of white, and is
then *abandoned* — from the next frame on NEW == OLD, the driver classifies the
pixel unchanged, and it is never driven again. The transition is stalled, not
slow.

**Fix:** remember which pixels changed over the last N frames and keep asserting
them as changed in the OLD plane (`OLD = NEW ^ mask`). Each pixel then gets N
drives instead of one.

```
frames -> ring of per-frame change masks (XOR of NEW and PREV), N deep
mask   -> OR of the last N
OLD    -> EPD_Display_OldPlane_Masked(NEW, mask)
```

**Costs zero waveform time** — same LUT, same frame count. The only cost is in
the OLD-plane write: 86 → 95 ms of SPI at N = 4, about 5% of the frame.

Measured on glass at MODE 10, N = 4: trail visible for ~2 frames, then fully
clear, white maintained, no flash anywhere.

### The reset phase must be zero when trail chasing is on

A re-driven pixel runs the **whole** LUT again, including the opposite-polarity
reset kick at the front. That replays as a dark flash on pixels that were already
clean, and because a multi-frame history staggers them, it reads as a **pulse
travelling behind the moving content**.

Set `reset_tp = 0`. Repeated drives supply the release the kick existed to
provide, so it costs nothing to drop — and it makes every mode two frames
(16 ms) faster.

## 7c. The floor at very low frame counts

MODE 1 is one drive frame: **8 ms at 15 V**, against the ~80–100 ms a full
transition wants. It cannot be fixed by waveform shaping, and frame rate does not
help either — 1 frame at 25 Hz and 5 frames at 125 Hz are the same 40 ms of drive
for the same busy time. Impulse is voltage x time, and time is what is missing.

The visible signature: a moving disc appears **squished along its direction of
travel**, because only the intersection of consecutive positions gets drive on
two consecutive frames. Everything outside that overlap has had a single 8 ms
push and is barely visible. It looks like geometric distortion; it is not.

The one remaining free lever is the source rails — VSH1 `0x41`→`0x4B` (15→17 V)
and VSL `0x32`→`0x3A` (−15→−17 V), ~13% more drive at an identical frame count
(`boost 1` in the lab). It will not rescue MODE 1, but it may move the usable
floor down a mode or two.

## 7d. Windowed transfer — works on the bench, NOT trusted. Leave it off.

BUSY does not depend on the window (the gate scan is always full-panel), so a
windowed write shortens only the transfer. Measured: 54400 bytes -> ~15000,
SPI 101 -> ~25 ms, MODE 10 from ~190 ms to ~115 ms end to end.

It renders correctly sometimes and produces stripes, boxes and flashing at other
times, and **the cause was never found**. `win 0` (the default) is the shipped
configuration. What is known:

- **The RAM address mapping is mirrored** and getting it wrong is the first thing
  to check on any partial-window work here. From the vendor full-screen setup:
  master X increments (chip col == buffer col); **slave X decrements**
  (chip col == 99 - buffer col); **both Y decrement** (chip row == 271 - buffer
  row). Writing buffer coordinates straight through addresses every row and the
  whole right half backwards. Fixed, and it was a real bug.
- **The window registers must be re-issued before EVERY plane**, not once per
  window. Rewinding only the address counters (0x4E/0x4F) between the NEW and
  OLD writes leaves the two planes misaligned.
- **Coverage is a hard requirement.** A pixel deliberately left with OLD != NEW
  (trail chasing) only settles when a later frame rewrites OLD = NEW for it. If
  the window moves off it first, it re-drives on every frame forever. The window
  must cover the whole history span, not just the frames being chased, and any
  static-but-changing region (a HUD, a score) needs its own window while its
  pixels are in flight.
- **Anything on screen that changes every frame destroys the whole idea.** A
  measured value in the corner puts those pixels in the change mask, so the
  window has to span the content AND the readout. Worse, it self-amplifies: a
  wider window costs more SPI, which changes the digits, which widens it again.
  Measured growth from 17 KB to 39 KB per frame before it was frozen.
- **The isolation test (`wintest`) shows the transport itself behaving**: writing
  OLD == NEW inside a window drives nothing inside that window, which is
  correct. The failure is in the surrounding baseline, which comes back
  inverted. Unexplained.
- `EPD_Update()` shipped `0xF7`, which the datasheet defines as DISPLAY in
  **3-color** mode; `0xFF` is the b/w equivalent. Corrected — but it made no
  observable difference, so it is not the cause of the above.

### Ruled out — do not re-test these

Each cost a round on glass. They are settled.

| hypothesis | verdict |
|------------|---------|
| Mirrored row / slave-column mapping | **Real bug, fixed.** chip row = 271 − buffer row; slave chip col = 99 − buffer col. Not sufficient on its own. |
| Window regs as `(min,max)` bounds instead of `(start,end)` | **Wrong.** Content lands at wrong addresses and jumps around. The part wants `(start, end)` in scan order — the vendor's literal form. |
| `0xF7` full refresh being the 3-color variant | **Real bug, fixed** (`0xFF` is b/w). Changed nothing observable here. |
| Coverage of trail-chased pixels | **Real requirement, implemented.** Not sufficient. |
| Bars are stale junk from earlier experiments | **No.** They regenerate in windowed mode and vanish on switching back to the full transfer. |
| Driving the panel white first | **No.** A full white *drive* leaves grey and the bars return. |
| Y-mirror inverted | **No.** Flipping it did not help. |

### The one fact that constrains everything

**A region that gets written comes out correct and stays correct. A region that
does not get written rots.** Switching back to the full transfer clears the bars
immediately and permanently; the moving content clears whatever it sweeps over.
So the damage is not in the bytes being written — it is that the un-written
regions do not hold what we believe they hold.

If picking this up again: stop reasoning about it and **read RAM back off the
chip** (`0x27`, read RAM) after a windowed write, then compare against the host
buffer. Every hypothesis above was plausible, several were genuinely true, and
none of them fixed it — the missing information is what the panel's RAM actually
contains, and that is measurable rather than arguable.

`wintest` is the harness for that: static, one update, no motion, no mask.

`spi hw 20` is an independent route to a similar saving and touches no addressing
at all. It is unproven on glass but is the cheaper thing to try first.

## 8. SPI is now the bottleneck

At MODE 8 the waveform is 89 ms but the frame is 175 ms, because **86 ms is
bit-bang SPI**. That is two full-panel plane writes at 43 ms each (true
differential writes NEW *and* OLD; the single-plane vendor path costs 43 ms).

Reducing waveform time below ~90 ms therefore buys progressively less. The next
real gains are in the transfer, not the waveform:

- windowed partial writes (`EPD_WriteWindow`) — only push changed rows
- hardware SPI instead of bit-bang (present in `spi.cpp`, **unproven on glass**)
- dropping the OLD-plane write where a true differential is not needed

---

## 9. Open items

- **Grey trails at low frame counts.** The VCOM fix cures the whole-panel
  darkening but not incomplete white transitions, which worsen as frames drop.
  This is impulse-limited.
- **The free lever for it is untested:** VSH1 `0x41`→`0x4B` (15→17 V) and VSL
  `0x32`→`0x3A` (−15→−17 V), ~13% more drive **at identical frame count**. Write
  with mask `WV_V_SRC` so EOPT stays out of it.
- **Trading frame count against frame rate at constant total drive time** —
  many short frames vs fewer long ones are not equivalent on the glass. Entirely
  unexplored.
- One BUSY timeout has been seen on the first update after a clear→seed→arm
  handoff. Recovers on its own; root cause not chased.

---

## 10. What the ecosystem does, and why it does not help

- **`bitbank2/bb_epaper`** has a native `EP579` driver. It uses the same fake
  100 °C temperature trick we do, and its fast path is `0xC7` — which is the
  wrong colour mode for this panel. No custom LUT.
- **`ZinggJM/GxEPD2`** (`GxEPD2_579_GDEY0579T93`) likewise: fake temperature,
  vendor waveform, no LUT work, no rails-up trick.

Neither library beats the OTP partial on this panel, and neither exposes the
frame rate or the activate byte. Everything above has to be done at the register
level.
