# Big Buck Bunny on the CrowPanel 5.79"

Delta-coded video streamed from a host over USB. Working as of 2026-08-09.

**Read the SETTLED RECIPE below before changing anything — mode 4 is the floor for
video, which is NOT what the game concluded.**

    python tools/encode_video.py C:\clip\frames bbb.bin --keyframe 60
    python tools/play_video.py COM7 bbb.bin --drive 3 --fps 24

Source is `C:\clip\frames`, 488 PNGs at 853x480.

## SETTLED RECIPE (2026-08-09, judged on glass: "looks great")

    encode_video.py <frames> bbb_c8.bin --rotate 90 --dither cluster8 --contrast 1.8 --step 2 --keyframe 60
    play_video.py COM7 bbb_c8.bin --drive 4 --old 1 --settle 1 --fps 12

| mode | LUT frames | busy | fps | on glass |
|------|-----------|------|-----|-------------|
| 1 | 2 | 16 ms | 21.5 | too light, no use |
| 2 | 3 | 24 ms | 20.4 | too light, no use |
| 3 | 4 | 32 ms | ~18 | too light, no use |
| 4 | 5 | 40 ms | 16.0 | **OK — the floor for video** |

**MODE 4 IS THE FLOOR FOR VIDEO. Do not carry the game's mode choice across.**

This is the sharpest instance of "content decides the waveform requirement". The game
settled on mode 2 as its value pick; on video modes 1-3 are all simply too light to be
usable, because a dithered photograph has no solid areas to carry it and every pixel is
transitioning every frame. The arithmetic that makes mode 1->2 look cheap (1.1 fps for
50% more impulse, since fixed costs dominate at the fast end) is real but irrelevant if
the whole range below mode 4 is unwatchable.

Consequence for real-time playback: mode 4 delivers 16 fps against a 24 fps source, so
step 1 would drag at 1.5x slow. Use **`--step 2` in the encoder and `--fps 12`** on the
player — 244 frames at 12 fps is the clip's true ~20 s duration, with 4 fps of headroom.

Tool options added while tuning. Encoder: `--rotate 90|270`, `--dither
bayer2|bayer4|bayer8|cluster4|cluster8`, `--contrast`, `--gamma`, `--step`,
`--keyframe`. Player: `--drive`, `--drives 3,4,5,6` (sweep, whole clip at each),
`--repeat N` (back to back in one session), `--old`, `--trail`, `--settle`, `--fps`,
`--no-pace`, `--loop`.

### The dither is a WAVEFORM constraint, not a cosmetic choice

The most transferable finding here, and nobody seems to write it down. All four
measured on glass with everything else held constant:

| dither | levels | result on glass |
|--------|--------|-----------------|
| `bayer8` | 64 | smooth gradients, **smears badly** |
| `bayer4` | 16 | **structured banding** |
| `cluster4` | 16 | resolves cleanly but **reads as blocky 4x4 texture** |
| `cluster8` | 64 | **the balance point** — clustered, 64 levels, 45 degree screen |

Mechanism: an isolated black pixel surrounded by white is the hardest case e-paper
has, because lateral field spread from its neighbours fights it, and at a short
waveform it never fully resolves. Dispersed matrices are nothing BUT isolated pixels,
so they smear. A regular lattice of half-resolved pixels reads as banding. Clustered
matrices grow compact blobs that do resolve, but an axis-aligned 4x4 grid is plainly
visible as texture — which is why print halftones are rotated 45 degrees, and why
`cluster8` (two diagonal dot centres per cell, 64 levels) is the one that works.

Corollary: **the same drive count that looks perfect on the game looks broken on
video**, because the game draws solid shapes with no dither lattice at all. Content
decides the waveform requirement — measured, not argued: the game is fine at mode 2,
video needs mode 4 minimum.

### `settle 1` is a real win on video — confirmed on glass

The VSS hold frame was removed early to double the game's frame rate, reasoning that
it exists to relax the pixel stack before the analog block powers down and the rails
never drop on the `0x0C` path. That holds for SPARSE updates. On video every pixel is
driven back to back with no idle time, which is exactly when an unrelaxed stack carries
charge into the next frame. Costs ~1.7 fps at mode 4 and earns it.

### `old 1` (true differential), not `old 0` and not `old 3`

- **`old 0`** (skip the OLD write) is the change that doubled the game's rate, and it
  is WRONG for video. Because the SSD1683 auto-promotes NEW to OLD, a pixel driven only
  part-way becomes `OLD == NEW` next frame and is **abandoned** mid-transition. RAM
  tracks intent, glass holds reality, and they diverge. This was the original cause of
  the heavy smearing.
- **`old 3`** (trail chasing) fixes the abandonment but writes a SYNTHETIC OLD plane
  (`NEW ^ mask`). On video the mask is nearly the whole panel, so every pixel is told
  it arrives from the opposite state, and with `reset_tp = 0` there is no kick phase to
  define that direction. Prime suspect for the frames that appeared to INVERT.
- **`old 1`** is the genuine previous frame: no abandonment, no fabricated state, and
  marginally faster than trail (no mask rebuild). Use this one.

### HUD overlay: cache it, do not re-rasterise

Two lines of 48 px text with white pads is ~40000 `Paint_SetPixel` calls per frame — a
switch, a seam test and a read-modify-write each — to redraw text that changes once a
second. Worth ~3 fps at mode 2 (17.4 -> 20.4).

The overlay sits at portrait y 36..148, and portrait y maps to native X as `799 - y`,
so it occupies native byte columns 81..95. The framebuffer is column-major, so **those
columns are one contiguous run** and the whole strip is a single 4624-byte memcpy.
Re-rasterised only when the text changes. See `SHUD_C0` / `SHUD_BYTES`.

### Host-side gotcha: set waveform options BEFORE `drive N`

`settle` is a waveform parameter, so it must be in place before the arm or the
`predicted_ms` banner reports a frame count the stream will not run at. It did still
take effect when sent afterwards (`set_wave_byte` re-arms), but the stale banner made a
valid A/B look invalid. `set_opts()` now runs before `set_drive()`.

## THE ONE THING TO KNOW: the link is the bottleneck, not the panel

The CrowPanel's USB-C is **not** wired to the ESP32-S3's native USB. Confirmed from
the vendor schematic for this board:

| net | goes to |
|-----|---------|
| `J2` (USB-C) D+/D- | `R3`/`R2` -> **`U1` = CH340C** |
| ESP32 `IO19`/`IO20` (native USB D-/D+) | `U5` (2x10 pin header) + pads `P32`/`P24` |

So the PC talks to a CH340C UART bridge, and the bridge talks to the ESP32 over a
serial line with a **baud rate**. Practical ceiling 2 Mbaud; 8N1 spends 10 bits per
byte, so **200 kB/s**.

The 4.26" sibling is a XIAO ESP32-S3, which has no bridge at all — its USB-C goes
straight to GPIO19/20 and it does **~1 MB/s**. Same SoC, 5x the pipe.

This is why the two projects feel so different:

| | 4.26" | CrowPanel 5.79" |
|---|---|---|
| link | native USB CDC, ~1 MB/s | CH340C UART, 200 kB/s |
| bytes/frame | 17853 | 6419 |
| link capacity | ~56 fps | ~31 fps |
| panel capacity | 12.4 fps | 24-33 fps |
| **limiter** | **the panel** | **the link** |

Fewer pixels does not help when the wire is the limit.

## NEXT SESSION: native USB (requires a soldered USB tap)

Wire a USB connector to the S3's native USB pins and the link stops mattering.

- **D- = GPIO19** -> header `U5` pad `P$5`, or test pad `P32`
- **D+ = GPIO20** -> header `U5` pad `P$16`, or test pad `P24`
- plus **GND**. 5V not needed; the board is already powered over its own USB-C.

**Verify the pins with a meter before soldering.** `P$5`/`P$16` are Eagle library
pad numbers for a 2x10 header and do NOT necessarily match the physical pin order
silkscreened on the board. Buzz continuity from the header pin to the ESP32 module's
IO19/IO20 and confirm which is which — swapping D+/D- just fails to enumerate, but
picking the wrong pins entirely could put USB signalling onto something else.

Firmware changes needed once wired, none of them large:

1. `platformio.ini`: `-DARDUINO_USB_CDC_ON_BOOT=1`. `Serial` then becomes the native
   USB CDC device on a NEW com port; the CH340 becomes `Serial0`.
2. `Serial.setTxTimeoutMs(0)` in `setup()`. On native CDC a write BLOCKS until the
   host drains it, so without this the frame rate depends on whether anything is
   listening — the 4.26" measured ~6 fps with a terminal attached and ~1 fps without,
   which looks exactly like a display fault and is not one.
3. **Delete the baud switching.** `Serial.updateBaudRate()` is meaningless on USB CDC.
   In `stream_run()` drop the `updateBaudRate(STREAM_BAUD)` / `updateBaudRate(115200)`
   pair, and in `play_video.py` drop `s.baudrate = STREAM_BAUD` and the matching
   restore. The ack handshake itself stays as is.
4. `Serial.setRxBufferSize()` is a HardwareSerial call. For USBCDC use
   `Serial.setRxBufferSize()` if present on the class, otherwise rely on the CDC
   driver's own buffering — but keep the ack flow control regardless.

Expect the panel to become the limit again: ~24 fps at mode 3, ~18 at mode 5.

## Two optimisations still owed, both mine, neither applied yet

Theoretical link capacity at 6419 B/frame is 31 fps. We measured 20.8. Most of the
gap is these:

1. **`STREAM_CHUNK` is 4096; it should be 12288 or more.** A porting error — the
   4.26" uses 12288. Its comment says chunk size "turned out not to affect throughput
   at all", but that was measured on native USB where a round trip is cheap. Over a
   CH340 every ack round trip pays USB's 1 ms frame quantum plus bridge latency, and
   at 4096 a frame costs 2-3 of them. Worth ~4-6 ms/frame, and it never shows up in
   the byte count. Raise it (32768 covers a whole frame) now that the rx buffer is
   64 KB.
2. **Keyframes and the checksum byte cost ~7%.** 5995 -> 6419 B/frame, 4.54x -> 4.24x.
   Added as insurance while the link was suspect. Now that `badck=0`, `--keyframe 0`
   is reasonable, or 120 as a compromise.

Do these BEFORE concluding anything about the remaining artefacts.

## Wire / file format

    header, once:  "CP79" | u32 frames | u16 w=800 | u16 h=272 | u32 plane=27200
    per frame:     3400 B bitmap, one bit per framebuffer byte, set = byte changed
                   N B    new value of each changed byte, in order
                   1 B    XOR checksum of bitmap + payload

`N` is never transmitted — both ends derive it by counting set bits, so there is no
length field to fall out of step.

A **keyframe** is just a frame whose bitmap is all ones. The format needs no flag for
it: the decoder already overwrites whatever it is told changed.

The checksum **must be read as part of the same transfer as the payload** — see the
bug list below.

## Geometry, and two panel specifics the encoder must honour

Source 853x480 (16:9) onto a 2.9:1 panel: scaled to the full **792 px visible width**
(853x480 -> 792x446), then the **top 272 rows kept** and the bottom 174 discarded.
A deliberate choice. Change the crop in `frame_to_fb()` if centring is wanted instead.

The encoder emits the panel's **own** buffer layout so the device transforms nothing:

1. **Column-major.** Index is `col * 272 + row`, not `row * 100 + col`.
2. **8-px dead gap at x = 396..403.** Two cascaded SSD1683s, 400 px each, 396 visible
   each. Source pixels >= 396 must be pushed 8 right or the entire right half lands
   8 px off. `Paint_SetPixel` does the same thing with `if (Xpoint >= 396) Xpoint += 8`.

Tone: gamma 1.3, contrast 1.4, fixed 4x4 Bayer. The dither **must** be ordered, not
error-diffused: a fixed matrix makes static areas byte-identical frame to frame so
they drop out of the delta entirely. Error diffusion perturbs every pixel and leaves
nothing to compress.

## Measured

488 frames, keyframe 60, checksum on:

| | |
|---|---|
| average | 6419 B/frame (4.24x) |
| median | 5430 B |
| p95 | 9156 B |
| worst | 30600 B (keyframe) |
| whole clip | 3.13 MB |

Without keyframes: 5995 B/frame, 4.54x, 2.93 MB.

| mode | busy | spi | tot | host fps | note |
|------|------|-----|-----|----------|------|
| 3 | 24 | 5 | 30 | 20.8 | wire-limited |
| 5 | 40 | 5 | 47 | 18.8-23.4 | near the crossover |

p95 of 9156 B at 25 fps is 228 kB/s, which exceeds 2 Mbaud — so ~1 frame in 20
arrives late even in the best case. The pacing clock recovers rather than
accumulating drift.

## Bugs found and fixed — worth not repeating

- **Checksum desync.** The device read payload and checksum as two separate
  `stream_read_exact` calls, each issuing one ack per chunk, while the host sent them
  as a single `npay + 1` stream. The device asked for one more chunk than the host
  ever sent, so it consumed the NEXT frame's bitmap as this frame's checksum and both
  ends were permanently offset. `badck` on 272 of 376 frames. On glass: blocky
  vertical bars, completely out of sync. **Fixed** by reading `paylen + 1` in one
  call, checksum at `pay[paylen]`. Both ends must chunk on identical boundaries.
- **UART rx buffer was the 256-byte default.** 1.28 ms of data at 2 Mbaud, so any
  time the receive task was descheduled by display work the FIFO overran and bytes
  vanished silently. In a delta stream that is unrecoverable, and because the buffer
  is column-major a shifted run is an 8-px **vertical stripe** — which is exactly the
  artefact that appeared after a few seconds and never healed. `setRxBufferSize(65536)`
  before `begin()`. The 4.26" had this all along; it was missed in the backport.
- **First ack lost across the baud switch.** The device changes rate and its receive
  task acks microseconds later, while the host may still be at 115200. A one-shot ack
  then deadlocks until the 4 s timeout kills the stream. The device now repeats the
  ack every 150 ms **while `have == 0` only** — mid-chunk a stray ack would
  desynchronise the host.
- **Phantom `ACT_RUN` after the restart clear.** The GC clear glitches the pulled-up
  button pins; a phantom `BTN_UP` is `ACT_RUN`, which dumped the panel into the bench
  demo instead of waiting for the host to re-enter. The restart path now clears
  `g_act` and forces `g_running = false`.
- **The device's own fps figure is not trustworthy.** Its timer spans the handshake
  AND a ~5 s post-stream read timeout after the host's last frame, so it reported
  17.1 where the host measured 20.8. Fix by starting `t0` on the first applied frame
  and stopping it on the last. Not done yet.

## HUD overlay

The fps counter composites from a **separate clean buffer** (`g_vid`), never into
`ImageBW` directly. Drawing the HUD into `ImageBW` would make the next delta land on
top of HUD pixels, and every byte under the text would be permanently wrong because
the host believes it is tracking pure video. Costs one 27 KB memcpy, ~1 ms.

## Mode switching during playback — PARTLY WORKING

Buttons LEFT/RIGHT during a stream change the mode. The intent is that the device
leaves stream mode, full-clears, skips the bar sweep, prints `STREAM restart drive=N`,
and the host re-enters and replays from frame 0.

**Not confirmed working on glass.** the operator reports the mode changes but the video does not
restart. The host now polls up to 9 s for the message (a single 1.2 s sleep missed it,
because the device first waits for its receive task to exit and then does a GC clear),
but that was never verified.

The bar sweep is deliberately skipped here: it parks the whole panel at the grey an
incomplete transition stalls at, which helps the game because there is a static
background, and hurts video because there is none — it would just lift the black floor
of every frame.

If the handshake keeps being awkward, the simpler design is a `restart` serial command
so the HOST drives the replay, with the buttons only setting the mode. No cross-device
handshake to get wrong.

## Still open

- **Smearing on video.** Expected: video changes nearly every pixel every frame, so
  incomplete transitions have nowhere to hide. Untested suspects, cheapest first:
  `settle 1` (the VSS hold frame was removed on the argument that the rails never drop
  on the `0x0C` path — true for sparse updates, questionable when every pixel is
  driven back to back); `old 1` (with auto-promote a pixel that lands short becomes
  `OLD == NEW` and is abandoned, so error compounds frame over frame); a coarser
  dither (Bayer scatters isolated single pixels, which are the hardest thing for
  e-paper to resolve against lateral field spread).
- **Residual vertical lines.** Fewer after the rx buffer fix, and `badck=0` proves the
  transfers are clean — so anything left is in the apply step or the display, not the
  link. The current checksum validates each TRANSFER but not the ACCUMULATED buffer. A
  full-frame XOR of `g_vid` compared against a host-computed expected value would
  separate "my buffer has drifted" from "the buffer is right and the panel renders it
  wrong". One byte per frame.
- **SD card is an alternative to all of this.** The 5.79 has a TF slot on
  `SCK 39 / MISO 13 / MOSI 40 / CS 10` (HSPI), no pin conflict with the EPD bus
  (11/12/45/46/47/48). See `../crowpanel-5.79-ref/example/arduino/Examples/5.79_TF/`.
  3.13 MB of clip fits trivially; that removes the wire entirely and the panel becomes
  the only limit. not taken up.

## Gotcha that costs time

**The board resets when the serial port closes.** Runtime settings do not survive
between tool invocations, so anything that must persist has to be a firmware default.
This is why `--drive N` is a `play_video.py` option rather than something you set once
with `serial_lab.py`.
