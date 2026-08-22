# Measuring this honestly

Timing on these panels is easy to measure: it is the BUSY pin, and it does not
lie. Image quality is not, and every wrong conclusion recorded during this work
came from a metric that was quietly broken rather than from a waveform that was.

This file is the list of ways the measurement fooled us. Read it before you
trust a contrast number, including one produced by the tools in this repo.

The metric itself: photograph the panel, take **p90 minus p10** of the greyscale
histogram inside a calibrated active area. Reference points on one panel were
solid white 141.0 and solid black 31.7, so a perfect result is around 109 and
the observed range was 13 (ink did not move) to 132.

---

## Trap 1 — a hand-picked crop measures the bezel

A crop chosen by eye scored contrast 93–100 for **every** drive count from 1 to
24. It made an 8 ms drive look as good as a 200 ms one.

The crop included the white plastic bezel and the test card's dark border line.
So p90 was plastic and p10 was a border line, and the histogram never saw the
ink at all. The metric was measuring the frame of the picture.

**The fix, and it must be automatic:** capture a solid white field and a solid
black field, difference them, and take the bounding box of what changed. That is
the active area, exactly. `tools/panelshot.py` carries the result.

**Never eyeball this crop.** A metric that cannot tell 16 ms from 200 ms will
happily confirm any waveform you like.

---

## Trap 2 — static solid regions dominate p10

Streaming a letterboxed 4:3 clip reported contrast 127 at drive 4, which looked
like an excellent result. It was not measuring the video.

The letterbox bars are **static**. They took one full transition, went solid
black and stayed there, and p10 sat in the bars. The moving content in the
middle of the frame was grey.

**The fix:** measure video with a full-frame pattern whose ink is all in motion.
A half-width black bar sweeping horizontally with wraparound is the right test
signal — every frame is exactly 50% ink and every pixel transitions twice per
cycle. Any static solid area in frame will flatter a bad waveform.

This is not a small correction. On the same panel, single-shot transitions
measured 114 / 116 / 121 / 121 at drives 2 / 4 / 8 / 16, and continuous video
measured 93 / 118 / 127 / 132. The fast end loses 21 points of contrast under
video that a single-shot test says is fine.

---

## Trap 3 — the first camera grab is cold

A cold capture-device open reads far too dark **even after discarding 40 warm-up
frames**: contrast 7–9, where the very next grab of the identical, unchanged
panel read 124–133.

One such reading was written down as "washed-out ghost, contrast 15" and drove a
default choice before it was caught.

**The fix:** throw the first grab away, always. `panelshot.py` does. And never
accept a single capture as evidence that a waveform failed — re-grab before you
believe a bad number, because the failure mode here is a *plausible* bad number,
not an obvious one.

---

## Trap 4 — the first update after arming is weak

The first custom drive after arming the LUT measured contrast 55 where every
repeat of the identical setting gave 95.

VCOM and EOPT take effect on the update **after** they are written. That is a
property of the part, not a bug in your code. Arm at boot; if you must arm
mid-run, throw the first frame away.

---

## Trap 5 — letting the readout drive the workload

A frame-rate counter drawn on the panel and updated every frame is a feedback
loop. A changed digit is a changed pixel; changed pixels widen the transfer
window; a wider window costs more SPI; more SPI changes the digits. Measured at
17 KB to 39 KB per frame once it was allowed to happen.

**Nothing downstream of a readout may decide when the readout updates.** Drive
it from a wall clock, upstream of everything.

The same rule, one step further: after a mode change, **blank the readout** for
its full period rather than showing the previous value. The banner settles in a
few frames and the meter takes a second, so a stale number sits next to the new
mode name and reads as a measurement. Blank is honest; stale is not.

---

## What a wall-clock number includes

Report the timed region and the wall clock separately, and say which is which.
On one port, MODE 10 measured 89 ms of waveform and 129 ms of timed region, but
6.8 fps wall clock rather than the 7.8 the arithmetic gives. The ~18 ms gap is
render, trail bookkeeping and the serial line — real work the panel waits on,
and the reason to measure wall clock at all rather than restating the sum above
it.

---

## When to stop

When two independent routes give the same number. The 91.2 ms analog ramp was
believable because a register scan, a difference of two vendor sequences, and a
streamed frame-rate delta all landed within 4 ms of each other. One measurement
that agrees with your expectation is the weakest evidence available.
