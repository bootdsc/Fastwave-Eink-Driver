# Open work

Honest list. These are the things that were measured and left unfinished, or
designed and never built. Several are probably worth more than anything already
in the repo.

## Line timing — the unexplored lever

Registers `0x3A` (dummy line period) and `0x3B` (gate line width) set line
timing directly, and **no firmware here writes either one**; they sit at
power-on defaults on every port. The init path sets the driver output control,
the border, the temperature sensor and the RAM window, and nothing else.

If the gate line width can be shortened, the frame period drops **below the FR
floor** — which is the only route to a faster waveform that this work has not
tried. Sweep it against `calib`, which will tell you immediately and cannot harm
the image.

This is the first thing to try if you want the number to go down.

## Windowed transfer

Real measured win on SPI (101 ms to 25 ms on one panel), correct rendering
*sometimes*, stripes and flashing at other times, and the cause was never found.
On a second panel sub-windows landed scattered across the glass.

What is known: you can window the **NEW** plane freely; anything requiring OLD
to differ from NEW must be full-plane. The mirrored addressing, the per-plane
window re-issue and the coverage rules are all worked out and documented in
[WINDOWED-TRANSFER.md](WINDOWED-TRANSFER.md).

Do not bring it back without reading RAM back over `0x27` to see where writes
actually land. Guessing has been tried.

## The ~17 ms of unaccounted per-frame cost on the SSD1683 port

A frame is 27200 bytes. The DMA path measures **97.5% of wire rate**, so the
transfer is ~22.5 ms of a measured 39 ms. Three separate attacks each bought
nothing: batching the RAM-window register writes, holding the SPI bus across the
session, and pipelining the DMA queue. Clock scaling proves it is fixed overhead
— 10, 20 and 40 MHz all leave the same gap.

Next step, and do not guess at it: time `EPD_SetRAMMP()` in a loop. There are
~30 small transactions per frame; if each really costs ~500 us that is the entire
gap, and it is a driver anomaly rather than a property of the panel.

## Flash-free multi-level grey

Designed, partially proved, not built. Four levels in one pass with no flash was
**measured at 178 ms** from a known white field. What is missing is the shadow
buffer and per-pixel transition codes that make it work for animation rather
than one-shot. Full design in [PICTURE.md](PICTURE.md).

Open question that might kill it: whether four true levels with per-pixel
transitions actually beats 1-bit with an 8x8 cluster dither, which gives 65
apparent levels, never flashes, and works today.

## Trail chasing under a custom waveform

Implemented on two ports and free on frame rate (`chase 0` and `chase 2`
measured identically over 30 seconds). Its effect on the soft-content case at
low drive counts is **unmeasured**. On the third port it was the fix for stalled
black-to-white transitions.

## Frame count versus frame rate at constant drive time

Many short frames against fewer long ones are **not** equivalent on the glass —
that much is known, because it is why one port ships FR `0x04` rather than the
fastest available code. The shape of that tradeoff has never been mapped.
Cheap to measure, likely to matter.

## Grey trails at very low frame counts

The VCOM fix cures whole-panel darkening but not incomplete white transitions,
which worsen as the frame count drops. One drive frame is 8 ms against the
~80–100 ms a full transition wants, so the fastest mode is impulse-starved by
construction and shows moving content squashed along its direction of travel:
only the overlap between consecutive positions gets driven twice.

Raising the source rails helps every transition equally and does not cure this.
Trail chasing is the candidate.

## DC creep over a long run

VCOM is at the value that fixed it on one panel. It has not been verified over a
long run on the others: stream for several minutes, refresh to white, and check
the field is still clean.

## One unexplained BUSY timeout

On the first update after a clear-seed-arm handoff, on one port. Recovers on its
own. Root cause not chased — but note that the recovery path is exactly where
the analog-rail ownership bug in [PITFALLS.md](PITFALLS.md) used to bite, so
check that first.
