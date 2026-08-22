# The waveform law

Everything in this repository rests on one equation and three register facts.
They were measured on three different SSD16xx controllers driving three
different panels, and they held on all three. The numbers change per part; the
shape does not.

If you read one file here, read this one.

---

## 1. What a "fast refresh" actually is

An SPI e-paper controller does not have a speed setting. It has a **waveform
table** — a list of voltage phases, each with a duration counted in *frames* —
and a **frame period** set by a clock divider. An update takes exactly as long
as the table says it will:

```
busy_ms = total_frames x frame_period_ms   ( + fixed costs you can remove )
```

`total_frames` is yours. `frame_period_ms` is yours. Neither is a property of
the picture.

**The vendor waveform is not tuned for your application.** It is tuned to be
safe across the part's whole temperature range, on any panel that controller
ships with, after any history of images, with no assumptions. That is why it
spends 44 frames where 9 will do, and why every measurement below is a factor
of 3 to 7 rather than a few percent:

| panel | vendor partial | this repo | ratio |
|-------|---------------:|----------:|------:|
| CrowPanel 5.79" (2x SSD1683, 792x272) | 336 ms | 48–89 ms | 3.8–7.0x |
| Seeed 4.26" (SSD1677, 800x480) | 540 ms full | 30 ms up | — |
| 1.54" (SSD1681, 200x200) | 363 ms | 72 ms | 5.0x |

On the 1.54" the custom waveform is also **better ink**: contrast 121 against
the vendor's 97. The stock partial is not a quality/speed tradeoff you are
undoing. It is slow *and* worse.

---

## 2. Content does not affect BUSY

Measured over hundreds of frames on the SSD1683: minimum, median and maximum
BUSY time are **identical**. A frame where one pixel changed and a frame where
every pixel changed cost the same.

This is the single most useful fact in the file, because it means:

- Timing is fully predictable before you render anything.
- Windowing the RAM transfer saves **SPI**, never waveform time.
- Reducing the gate count (MUX) does not shorten a frame. Measured on the
  SSD1677 at MUX 480 / 240 / 120 / 60: 9921 us every time.
- A 200-line panel and a 272-line panel share the same 8.06 ms frame period.
  A smaller panel buys you transfer time, not drive time.

Anything that claims to make the *waveform* faster by drawing less is wrong on
these parts.

---

## 3. The frame-count equation

Waveform tables are groups of phases. Each group carries phase durations and
repeat counts, and the frame count is a product of them. On the SSD1681, with
the group byte layout `[TPA, TPB, SRAB, TPC, TPD, SRCD, RP]`:

```
frames = (RP+1) * ( (SRAB+1)*(TPA+TPB) + (SRCD+1)*(TPC+TPD) )
```

**Every repeat field is n+1: a stored 0 runs the group once.** That is the
easiest thing in this entire subject to get wrong, and it is invisible when you
get it wrong — BUSY stays perfectly linear in your parameter while the waveform
actually runs 2x or 4x longer than you intended. A clean straight line is not
evidence that your decode is right.

Two independent confirmations are what settled it:

- A null table of `tp,0,1,0,0,0,1` measured `4 x tp` frames. Decoded:
  RP=1 means 2, SRAB=1 means 2, so `2 * (2*tp) = 4tp`. Exact.
- The first working waveform, `1,0,5,1,0,1,1`, measured 15.94 frames. Decoded:
  `2 * (6*1 + 2*1) = 16`. Exact.

And it then predicted busy at drive 1/2/3/4/6/8/12/16/24 as
16/24/32/40/56/72/104/137/201 ms against measured
16/24/32/40/56/72/104/137/201 ms.

**The byte order is per-part and must be measured.** The SSD1681 is TP-first as
above; the SSD1683's table reads RP-first. A full sweep under the *wrong*
assumed order also produced a clean linear fit. Only perturbing one byte at a
time separates them — see [PROBES.md](PROBES.md), `bytescan`.

### Total length is the maximum over blocks

The transition blocks share one timeline. Adding phases to an otherwise-idle
block costs nothing.

### Block order (SSD1681 and SSD1683 agree)

`0` = LUTC (VCOM), `1` = WW, `2` = BW, `3` = WB, `4` = BB.

The two **changing** classes are 2 (ends white) and 3 (ends black). Blocks 1
and 4 must stay at zero, or static content gets driven every update. Driving 1
and 2 by mistake pushes every unchanged white pixel toward black and wipes the
image to a uniform grey — which is how the convention was identified.

VS level encoding: `00` = VSS (no drive), `01` = VSH1, `10` = VSL, `11` = VSH2.
In the VCOM block, `11` is float.

---

## 4. `0x22` is a bitfield, not a menu of magic bytes

Every vendor driver for these parts contains a line like `write(0x22, 0xCF)`
with no explanation, and every fork of that driver copies it. It is a bitfield:

| bit | meaning |
|-----|---------|
| 7 | enable clock |
| 6 | enable analog |
| 5 | load temperature value |
| 4 | **load LUT from OTP** |
| 3 | LUT / mode select — **meaning is part-specific** |
| 2 | **DISPLAY** |
| 1 | disable analog |
| 0 | disable clock |

Three consequences, each worth a large fraction of your frame budget:

**(a) Ramping the analog rails per update is a fixed tax.** Measured with a
null waveform so the drive time is identical:

| activate byte | SSD1681 busy | what changed |
|---------------|-------------:|--------------|
| `0x04` display only | 72.6 ms | baseline |
| `0x0C` display + mode select | 72.6 ms | nothing |
| `0xC4` + analog ramp up | 163.8 ms | **+91.2 ms** |
| `0xCF` + ramp up and down | 305.0 ms | **+232 ms** |

Bring the rails up **once** with `0xC0`, then fire bare `0x0C` for every frame.
The fixed cost then measures 0 ms and busy is exactly `frames x period`. The
same trick on the SSD1683 removed 91–124 ms per update.

**(b) Bit 4 silently discards your custom waveform.** `0x1C`, `0xFC`, `0xD7`,
`0x91` all set it, so all of them reload the factory table from OTP. If your
carefully built LUT stops working the instant you call a vendor helper, this is
why. Re-arm the LUT after **any** byte with b4 set.

**(c) Bit 3 does not mean the same thing on two parts.** On SSD1677 it selects
DISPLAY Mode 1 (full) vs Mode 2 (differential). On SSD1683 it selects 3-colour
vs black/white — so `0xC7`, which several libraries ship as a "fast path", is
DISPLAY in 3-colour mode on a b/w panel. That is the inversion and cascade
asymmetry it has always produced. **Do not port bit 3 between parts.** Measure
it.

---

## 5. The frame period comes from the FR field

A four-bit code per group, packed two groups to a byte. On both the SSD1683 and
the SSD1681 it is the same table, measured nine values deep:

| FR code | nominal | measured us/frame |
|---------|---------|------------------:|
| `0x01` | 25 Hz | 40321 |
| `0x09` | 37.5 Hz | 26862 |
| `0x02` | 50 Hz | 20159 |
| `0x0A` | 62.5 Hz | 16118 |
| `0x03` | 75 Hz | 13437 |
| `0x0B` | 87.5 Hz | 11508 |
| `0x04` | 100 Hz | 10080 |
| `0x0C` | 112.5 Hz | 8953 |
| `0x05` | 125 Hz | 8063 |

Every code lands 0.8% low — one clock offset, not scatter. `0x06`–`0x08` are
NA and the firmware rejects them. The SSD1677 uses a different encoding
entirely: `0x88` gives 9.92 ms, and the OEM tables ship `0x22` at 39.6 ms —
four times slower for no visible benefit.

**Pick FR and drive count as a pair, not in sequence.** Fewer, longer frames
switch the ink harder than more, shorter ones for the same total time. On the
1.54", FR `0x04` at drive 5 gives 60.5 ms and ~15 fps; FR `0x05` needs drive 8
and 72.6 ms for equal contrast. Faster *and* darker. This is the one knob in
the repo whose two axes are not independent, and the reason it ships at `0x04`
rather than the fastest available code.

---

## 6. The costs that are not frames

| what | cost | fix |
|------|-----:|-----|
| EOPT at power-on-reset (`0x3F = 0x22`) | +40.3 ms on a 200-gate panel, +17 ms on 272 gates | write `0x3F = 0x07` |
| analog ramp per update (`0x22` b6) | +91.2 ms | hold the rails up |
| ramp down (`0x22` b1+b0) | +141 ms | never per frame |

EOPT `0x22` is two discharge *scan frames*, so its cost scales with gate count.
It is the POR value, which means **every stock driver on the part is paying
it**, and it is one register write to remove. Measured under a null LUT: slope
unchanged at 8064 us/frame, intercept 48339 us to 8088 us.

On the SSD1683, EOPT is the opposite call — including it costs +17 ms *and* a
visible flash, so that port leaves it out of the voltage mask entirely. Same
register, different answer, because the panels differ. Measure it.

---

## 7. Voltage is free contrast

`VSH1` / `VSL` set how *hard* a pixel is driven, not how *long*. Raising the
source rails from the POR +/-15 V (`0x41` / `0x32`) to the datasheet maximum
+/-17 V (`0x4B` / `0x3A`) is roughly 13% more drive per frame at an **identical
frame count** — confirmed on glass, busy stayed at exactly 89 ms and 48 ms with
the boost on.

The benefit *grows* as the mode gets faster, because a short drive is
impulse-starved and voltage is the only other axis. This is the cheapest
improvement in the repo.

Two boundaries:

- **The datasheet maximum is the maximum. Do not go past it.** Above it, frame
  rates do not improve at all and the extra contrast is marginal — the question
  is closed, and there is nothing up there worth the risk to the panel. No
  register values for it appear anywhere in this repository, and every port's
  `volts` command refuses an out-of-spec value rather than writing it — so a
  mistyped hex digit is an error message, not an overvoltage.
- **Voltage is not persistent.** A reset reverts to POR. So does anything that
  pulses RST, including your own recovery paths.

`VCOM` is not a contrast control, it is a **DC-balance** control. See
[PITFALLS.md](PITFALLS.md) §"the panel creeps dark".

---

## 8. Where the time actually goes

Once the waveform is short, it stops being the whole story:

```
frame_ms = (drive + settle) x frame_period + SPI + host/USB
```

- **SPI is per frame and unavoidable** on parts with no plane-copy command: the
  OLD plane must be rewritten every update even when it is simply last frame's
  NEW plane.
- **SPI is a fixed cost, so it dominates short modes.** On the SSD1677, going
  20 to 40 MHz gains 12% at drive 7 and 21% at drive 2.
- **Driver overhead can exceed wire time.** On the SSD1683, a frame is 27200
  bytes — about 11 ms of wire at 20 MHz — but measured 39 ms, of which ~17 ms
  is fixed per-frame driver cost that does not scale with the clock at all
  (10 / 20 / 40 MHz all leave the same gap). Clock scaling is how you tell
  transport cost from overhead. Do this before optimising the wrong one.
- The remaining overhead on that port was traced to per-byte function-call cost
  in the burst writer, not the bus: the panel wants column-major bytes from a
  row-major framebuffer, and doing that one strided read at a time cost ~110
  cycles per byte. Storing the framebuffer column-major turned the transfer
  into a `memcpy` and took SPI from 101 ms (bit-bang) to 40 ms.

**Set the SPI clock once, at boot, and never rebuild the device while the panel
is live.** Rebuilding it mid-traffic wedges the SSD1677: transfers still
complete, the controller stops acting on commands, `busy_ms` reads 0, and only
a full board reset clears it. 26.7 and 40 MHz both wedge when switched into at
runtime and both are stable when they are the only speed the chip ever sees.
The ESP32 divides 80 MHz by an integer, so the real choices are 20, 26.7 and
40.

---

## 9. The order to do this in

1. Run `calib` — get the frame period per FR code. **Do not assume 8.11 or 9.92
   ms.**
2. Run `seqscan` — find the fixed costs and which activate byte avoids them.
3. Run `bytescan` — settle the group byte order. Do not trust a linear fit.
4. Build a null LUT and confirm it changes nothing on the glass at full time
   cost. Now you have a timing model with no risk to the image.
5. Only now build a real waveform, and calibrate your *measurement* before you
   trust any contrast number — see [MEASURING.md](MEASURING.md).

Steps 1–4 cannot alter the image. That is the entire point of them.
