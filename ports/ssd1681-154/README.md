# SSD1681 — 1.54", 200x200

The smallest of the three, and the clearest demonstration that the vendor
waveform is not a tradeoff.

**Vendor OTP partial: 363 ms, contrast 97. A 9-frame custom waveform: 72 ms,
contrast 121. Five times faster and better ink.**

| | |
|--|--|
| panel | 1.54" mono, 200x200 (GDEH0154D67 / HINK-E154A07-A1) |
| controller | SSD1681 |
| MCU | XIAO ESP32-S3, native USB (TinyUSB CDC) |
| serial | 115200 |
| planes | 5000 bytes each |

**Check the flex-cable pinout against your breakout before plugging the panel
in.** A 24-pin e-paper connector is not a standard, and the two boards this was
tried on did not agree.

## Build and flash

```
python -m platformio run -d .
python ../../tools/flash.py COMx xiao-154
```

The flash tool has **no default environment**, deliberately: an image built for
an 800x480 panel driving this 200x200 one cost a whole session of "waveform"
debugging. The firmware prints its panel identity in `STATUS` and in the stream
banner, and the host repacks to match or refuses.

## Measured, on glass

Single-shot white to test card, custom waveform, FR `0x05`, POR rails, VCOM
`0x08`, EOPT `0x07`. Contrast is p90 − p10 inside a calibrated active area;
solid white reads 141.0 and solid black 31.7.

| drive | frames | busy | streamed fps | contrast |
|-------|-------:|-----:|-------------:|---------:|
| 1 | 2 | 16 ms | — | **13** — below threshold, the ink does not move |
| 2 | 3 | 24 ms | 34.5 | 114 |
| 4 | 5 | 40 ms | 22.2 | 116 |
| **8** | **9** | **72 ms** | **13.0** | **121** (saturated) |
| 12 | 13 | 104 ms | 9.1 | 121 |
| 16 | 17 | 137 ms | 7.0 | 121 |
| OTP `0x1C` | ~44 | 363 ms | 2.8 | 97 |
| OTP `0xF7` full | — | 2132 ms | 0.5 | 91 |

Streamed frame rate tracks the timing law to within 3% at every point, so this
panel is purely waveform-limited — SPI, USB and host encode all hide behind the
drive.

### Continuous video is a different floor from a single transition

Measured with a half-width black bar sweeping horizontally with wraparound, so
every frame is exactly 50% ink and every pixel transitions twice per cycle:

| drive | busy | streamed fps | video contrast |
|-------|-----:|-------------:|---------------:|
| 2 | 24 ms | 34.4 | 93 |
| 4 | 40 ms | 22.2 | 118 |
| 8 | 72 ms | 12.9 | 127 |
| 16 | 137 ms | 7.0 | 132 |

Single-shot at the same settings gave 114 / 116 / 121 / 121, so video costs real
contrast at the fast end and almost nothing from drive 8 up. Pick by content:

- **drive 2, 34 fps** — high-contrast graphics only
- **drive 4, 22 fps** — solid-edged / graphic video
- **drive 8, 13 fps** — dithered photographic video. The shipped default.

## Defaults, and why FR is 0x04 and not 0x05

```
fr           0x04     (100 Hz, 10.08 ms/frame)
drive        8
eopt         0x07     (POR 0x22 costs +40.25 ms on every update)
vcom         0x08
activate     0x0C     (rails held up; anything with bit 4 reloads OTP)
```

`0x05` is the fastest available code and is **not** the right one. The 25%
longer pulse at `0x04` switches the ink visibly harder, so drive 5 at `0x04` is
60.5 ms / ~15 fps against drive 8 at `0x05`'s 72.6 ms / 13.0 fps — faster *and*
darker. Frame rate and frame count have to be chosen as a pair.

EOPT here is the opposite call from the SSD1683 port, where the same register
costs +17 ms and a visible flash. On a 200-gate panel its two discharge scan
frames cost 40 ms, and it is the power-on default, which means **every stock
driver on this part is paying it**.

## Serial

```
help  status  reboot  download
white  white1  black  card  card inv  scrub N    full-refresh stills
blink N  frame  stream
seq X  trail N  chase N
calib  seqscan  bytescan
```

`white` is a black/white deghost cycle; `white1` is a single refresh to white
with no black flash, which is what stream entry uses.

## The bring-up ladder

Each rung removes one suspect. Do not skip to the top.

```
card      device renders its own test card with the full vendor sequence
blink 6   device flips card/inverse with YOUR sequence, printing busy_ms
frame     one host frame over the wire, applied with the full vendor sequence
stream    host frames applied with your sequence
```

`card` lands but `blink` does not → the sequence is at fault. `frame` lands but
`stream` does not → same conclusion from the other side. `card` lands but
`frame` does not → the wire or the host packing, and the panel is fine.

## What a smaller panel actually buys

Not waveform time. Against the 5.79" at 5.4x the pixel count:

| | 5.79" | 1.54" |
|--|--:|--:|
| frame period at FR `0x05` | 8.11 ms | 8.06 ms — **no gain** |
| SPI, both planes | 13 ms @ 40 MHz | 4 ms @ 20 MHz |
| 5-frame wall clock | 62 ms → 16 fps | 52 ms → 19 fps |

Expect ~1.2x, not 5x. The frame period is a frame-rate target, not a function of
gate count, and BUSY has no content dependence. Fewer pixels buys transfer time
and trail-chase headroom, and nothing else.

## Read next

[docs/panels/SSD1681-1.54.md](../../docs/panels/SSD1681-1.54.md) — the full
record, including the `bytescan` result that settled the group byte order and
the three metric traps that produced wrong answers before it was calibrated.
