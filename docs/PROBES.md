# The probes

Four measurements. Three of them **cannot change what is on the glass**, which
is what makes them safe to run first on unfamiliar hardware — including hardware
whose datasheet you do not have.

Every one of them works by building a **null waveform**: a table whose phases
all sit at VS level `00` (VSS). It takes the full time to run and puts zero
field across the pixel. The panel spends the milliseconds and the image does not
move. If your null waveform changes the picture, your VS encoding is wrong and
you have learned that before risking anything.

The implementations are `wave1681_build_null()` / `wave1677_build_null()` and
the `calib` / `seqscan` / `bytescan` / `probe` serial commands.

---

## `calib [fr]` — the frame period and the fixed cost

Build a null table where one group runs `TP` frames. Sweep `TP`. Least-squares
the results:

- **slope** = frame period, in microseconds per frame
- **intercept** = the fixed cost per update that is not frames

Then repeat per FR code, to prove the FR field is actually being honoured rather
than ignored in favour of an OTP default.

This is the first thing to run on any new panel, and the numbers it returns are
not portable. Two panels a factor of 1.9 apart in pixel count returned the same
8.06 / 8.11 ms period; a different controller in the same family returned 9.92
ms from a completely different FR encoding. Assume nothing.

Worked example, SSD1681, FR `0x05`:

| EOPT | slope | intercept |
|------|------:|----------:|
| `0x22` (POR) | 8065 us/frame | 48339 us |
| `0x07` | 8064 us/frame | **8088 us** |

Same slope, 40.3 ms of intercept gone. That is what a fixed cost looks like when
you find one: the slope does not move.

---

## `seqscan` — what each activate byte costs

Same null waveform, fixed frame count, vary **only** the `0x22` activate byte.
Every difference between rows is power sequencing, because the drive is
identical by construction.

| activate | busy | reading |
|----------|-----:|---------|
| `0x04` | 72595 us | display only |
| `0x0C` | 72597 us | display + mode select — no cost |
| `0x84` | 72594 us | + clock enable — no cost |
| `0xC4` | 163845 us | **+ analog ramp, +91.2 ms** |
| `0xCF` | 305033 us | + ramp down, +141 ms more |

Two rules for this probe:

- **Refuse any byte with b4 set.** `0x1C`, `0xFC`, `0xD7` reload the OTP table,
  so you would be timing a different waveform and comparing it to yours. The
  firmware here rejects them rather than reporting a nonsense row.
- After the probe the resident LUT is the null table. It drives nothing. Any b4
  sequence, or `white`, restores the factory table.

`seqscan` is also how you cross-check yourself. On the SSD1681 the 91.2 ms ramp
showed up three independent ways: in this scan, in the difference between the
two OTP partial sequences (`0xFC` − `0x1C` = 95 ms), and in a streamed frame
rate difference (91 ms). Three routes to one number is when you can stop.

---

## `bytescan` — the group byte order

The only probe that answers a question a linear fit cannot. Perturb **one** byte
of one group at a time from a known baseline and read the frame count.

Baseline `1,8,0,0,0,1,0` = 9 frames, probe value 4:

| byte | table | frames | conclusion |
|------|-------|-------:|------------|
| — | 1,8,0,0,0,1,0 | 9 | baseline |
| 0 | 4,8,0,0,0,1,0 | 12 | TPA, additive |
| 1 | 1,4,0,0,0,1,0 | 5 | TPB, additive |
| 2 | 1,8,4,0,0,1,0 | 45 | SRAB, multiplies by 5 — so **n+1** |
| 3 | 1,8,0,4,0,1,0 | 17 | TPC, additive, x2 because SRCD=1 means 2 |
| 4 | 1,8,0,0,4,1,0 | 17 | TPD, same |
| 5 | 1,8,0,0,0,4,0 | 9 | SRCD — no effect, TPC=TPD=0 |
| 6 | 1,8,0,0,0,1,4 | 45 | RP, multiplies by 5 — so **n+1** |

Only one interpretation reproduces all seven rows. **A full FR sweep under the
wrong assumed byte order also produces a clean linear fit** — that is the trap
this probe exists to close. A slope that looks right is not evidence that the
layout is right.

Keep a decoder that can read a table under either candidate order, so you can
compare rather than argue.

---

## `probe B L T` — which block drives which transition

Drive exactly one LUT block, with one level, for `T` frames, and look at the
glass. This is the one probe that *does* change the image, and it is the only
way to identify the transition mapping on a part whose documentation disagrees
with itself.

What it settles: which block index corresponds to white-to-white,
black-to-white, white-to-black and black-to-black, and therefore which two you
are allowed to touch. Getting it wrong drove every unchanged white pixel toward
black and wiped a test card to uniform grey — a very legible failure, and much
cheaper to produce deliberately than to debug in a real waveform.

---

## The bring-up ladder

Each rung removes exactly one suspect. Do not skip to the top; a stream that
fails at the top with three untested layers below it is not debuggable.

```
card      the device renders its own test card with the full vendor sequence.
          The host is not involved at all.
blink N   the device flips card/inverse using YOUR sequence and prints busy_ms
          per frame. Answers "does my sequence move ink".
frame     one host-supplied frame over the wire, applied with the full vendor
          sequence. Answers "is the wire and the packing right".
stream    host frames applied with your sequence. Everything at once.
```

Reading it:

- `card` lands, `blink` does not → your activate sequence is the fault. Try a
  self-powered byte against a rails-up one.
- `frame` lands, `stream` does not → same conclusion, from the other side.
- `card` lands, `frame` does not → the fault is the wire or the host packing,
  and the panel is fine.
- `card` does not land → stop. Pins, power rail, FPC seating, BUSY polarity.

---

## Serial hygiene

Open the port with **DTR and RTS deasserted**. A default terminal connect
resets the board, and a reset drops the voltage registers, the resident LUT and
the image. The panel keeps all three across a *reconnect* and none of them
across a *reset*, so the difference decides whether attaching a terminal
destroys the run you were measuring.

`tools/serial_lab.py` and `tools/peek.py` both do this. A plain terminal does
not.
