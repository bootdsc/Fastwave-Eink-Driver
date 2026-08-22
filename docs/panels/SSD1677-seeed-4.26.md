# SSD1677 — Seeed 4.26" 800x480, measured

Fast custom waveforms and video playback on a **Seeed 4.26" 800x480 mono e-paper**
(GDEQ0426T82 / SSD1677) driven by a **XIAO ESP32-S3** on the Seeed ePaper
Breakout Board v2.

Sibling of the [SSD1683 port](SSD1683-crowpanel-5.79.md), which did the
same for the dual-SSD1683 CrowPanel 5.79". The SSD16xx groundwork carries over —
the `0x22` bitfield, the rails-up trick, the `calib`/`seqscan` probes — with one
meaning changed: on SSD1677 bit 3 of `0x22` selects DISPLAY Mode 1 vs Mode 2, not
3-colour vs black/white.

## What works

- **Custom waveforms** via `0x32` (105 bytes, 10 groups x 4 phases), replacing the
  factory LUT. Vendor full refresh is 540 ms; the bench runs differential updates
  from 30 ms up.
- **Video playback** from a host PNG sequence, dithered to 1 bit, delta-compressed
  over USB, at up to **12.4 fps** on an 800x480 panel with no flashing between
  frames.
- **Native 4-level greyscale** for stills (`gstill`), with Bayer dithering applied
  between adjacent levels.

## Timing model

    frame_ms = (drive + 1) x 9.92 + 36

- `(drive + 1) x 9.92` — waveform time. FR code `0x88` gives 9.92 ms/frame, the
  fastest `calib` found; the OEM tables ship `0x22` (39.6 ms), four times slower
  for no visible benefit.
- `36` — a flat per-frame cost: 20 ms of SPI (both RAM planes, 96000 bytes at
  40 MHz) plus ~16 ms of delta-apply and USB handshaking. Fitted to measurement,
  not assumed — frame time minus waveform came out 35.8/35.8/35.9/36.1/36.1/36.9
  across drives 12/9/8/7/5/2.

SPI is unavoidable per frame because the controller has **no plane-copy command**,
so the old plane must be rewritten every time even when it is simply the previous
new plane.

Measured at 40 MHz SPI, and at the 20 MHz the panel is specced for:

| drive | 20 MHz | 40 MHz | | drive | 20 MHz | 40 MHz |
|-------|--------|--------|-|-------|--------|--------|
| 1     | —      | 17.44  | | 7     | 7.68   | 8.66   |
| 2     | 12.42  | 15.00  | | 8     | 7.16   | 7.99   |
| 3     | —      | 13.08  | | 9     | 6.71   | 7.41   |
| 4     | 9.92   | 11.64  | | 10    | 6.27   | —      |
| 5     | 9.04   | 10.46  | | 12    | —      | 6.07   |

The gain grows as drive falls — 12% at drive 7, 21% at drive 2 — because SPI is a
fixed cost and therefore a larger share of a short waveform.

## SPI at 40 MHz

The SSD1677 is specced at 20 MHz. It runs clean at **40 MHz** (80/2), halving SPI
from 39 ms to 20 ms with BUSY unchanged, on the breakout board with jumper leads.

The one rule: **set the clock at boot and never change it at runtime.** Rebuilding
the SPI device while the panel is live wedges the controller — transfers still
complete but it stops acting on commands, with `busy_ms=0` the only symptom, and
only a full board reset clears it. That is a property of the mid-traffic rebuild,
not of the clock: 26.7 and 40 MHz both wedged when switched into at runtime and
both are stable when the controller only ever sees one speed. `spi hw N` is
therefore clamped to 20; the boot value in `setup()` is the one that matters.

Note the ESP32 divides 80 MHz by an integer, so the only choices are 20 (80/4),
26.7 (80/3) and 40 (80/2) — `spi hw 24` through `spi hw 32` all resolve to 26.7.

## Video streaming

The clip cannot live on the device — 488 frames of packed 800x480 mono is 23 MB
against 8 MB of flash — so the host decodes, crops, dithers, packs and
delta-encodes, and the device only applies and displays.

Three things had to be true to reach 12 fps, and each was found by measurement
rather than reasoning:

1. **The CDC receive buffer defaults to 256 bytes.** A 4096-byte chunk arrived as
   1205 bytes, the rest silently dropped. `setRxBufferSize(65536)` before
   `begin()`.
2. **`Serial.readBytes` reads one byte at a time.** It is `Stream::readBytes`, so
   a frame cost 48000 queue operations. Bulk `Serial.read(buf, n)` instead.
3. **USB and the panel ran in series.** USB alone measured 4.9 fps, the panel
   alone 8.6, and together exactly 3.1 = 1/(1/4.9 + 1/8.6). Receiving on a task
   pinned to the other core, into one of two ping-pong buffers, makes the slower
   of the two set the rate instead of their sum.

Then compression removed USB as a constraint entirely — see below.

## Compression

Each frame is sent as a **changed-byte bitmap plus the new values of the changed
bytes**. The count of changed bytes is never transmitted; both ends derive it by
counting set bits, so there is no length field to fall out of step.

Measured on Big Buck Bunny at step 3, ~3.4x smaller. It works because the 4x4
Bayer matrix is **positionally fixed**: a static area dithers to byte-identical
output every frame and drops out of the delta completely. Error-diffusion
dithering would perturb every pixel each frame and leave nothing to remove.

Alternatives measured and rejected:

| encoding                | B/frame | ratio | note                  |
|-------------------------|---------|-------|-----------------------|
| raw                     | 48000   | 1.00x |                       |
| dirty 64-byte blocks    | 44921   | 1.07x | dither noise touches nearly every block |
| XOR + RLE               | 18920   | 2.54x |                       |
| **changed-byte bitmap** | **17853** | **2.69x** | **trivial decoder** |
| XOR + deflate           | 10094   | 4.76x | needs inflate on-device |

Deflate wins on ratio and is still the wrong choice: with the bitmap the link
measures **18 fps** against a 12.4 fps best case from the panel, so the wire is no
longer the bottleneck and a decompressor would buy nothing. The remaining levers
are the 40 ms of SPI and the 10.8 ms of overhead.

## Tools

    tools/play.py COM19 D:\clip\frames          # video, drive auto-selected
    tools/play.py COM19 image.png               # still, 1-bit dithered, drive 12
    tools/play.py COM19 image.png --gray        # still, native 4-level grey
    tools/play.py COM19 image.png --rotate 270  # portrait source, panel on side
    tools/play.py COM19 frames --no-pace        # free-run, for benchmarking
    tools/play.py COM19 frames --sink           # measure the link with no panel
    tools/mirror.py COM19 --select              # mirror a live screen region
    tools/webcam.py COM19                       # integrated webcam, StarPilot portrait
    tools/webcam.py COM19 --list                # camera indices
    tools/webcam.py COM19 --landscape           # panel flat / wide
    tools/analyze_compress.py D:\clip\frames    # compare encodings offline
    tools/spi_sweep.py COM19                    # SPI clock vs whether it refreshes
    tools/serial_lab.py COM19 out.log "cmd" ... # scripted serial, no board reset
    tools/peek.py COM19 "status"                # read raw output, no reset

Playback is **paced to real time** by default, at `source_fps / step`. Without a
clock the host ships frames as fast as the panel accepts them, so a mode faster
than the source needs plays sped up rather than correct — drive 2 at 14.9 fps
against a 12.5 fps requirement runs 19% quick. `--no-pace` restores free-running
for measurement. A mode SLOWER than the requirement simply falls behind; no clock
can fix that, and this one does not pretend to.

Images fill the panel by centre-crop; `--fit` letterboxes instead, and `--rotate`
turns the source first for a panel stood on its side.

`webcam.py` is the live camera path: capture on a thread (drop old frames),
Bayer + delta on the host, same `stream` handshake as `play.py`. Native USB CDC
— no 2 Mbaud hop, which was the CrowPanel CH340 ceiling. Default is StarPilot
portrait (rotate 270) and drive 2 (~15 fps, panel-limited). Live keys in that
window: `[` `]` / `1-9` drive, `,` `.` volts, `g/c/h/k` tone, `i` invert,
`p` pause. Drive/volts leave stream for ~4 s then rejoin (the device cannot
take commands mid-stream). Outer 1 px is forced white so live ink never hits
the rim.

`serial_lab.py` deasserts DTR/RTS before opening, so attaching does **not** reset
the board — which matters because the panel keeps its image and its voltage
settings across a reconnect but not across a reset.

Drive mode is chosen from `--source-fps` as the *slowest* mode that still clears
`source_fps / step`: slower modes are darker and higher-contrast, so overshooting
costs picture quality while undershooting makes playback drag. 25 fps at step 3
lands on mode 6, 30 fps at step 4 on mode 7.

## Tone defaults

Ordered dithering reads washed out because the Bayer threshold is a flat ramp:
any source sitting above mid-grey turns most cells white. All values judged on
glass.

| use    | gamma | contrast |
|--------|-------|----------|
| video  | 1.3   | 1.4      |
| stills | 1.55  | 1.4      |
| grey   | 1.2   | 1.4      |

`--highlights` compresses only the range above `--knee`, leaving shadows exactly
as they were — the right control when bright points need to come down but dark
detail is already correct. Gamma cannot do that; it moves everything.

## Voltages

`volts VSH1 VSL` in hex, applied globally — there is no per-line control.

- Stock `41 32` = +/-15 V
- **Default `4B 3A` = +/-17 V**, the datasheet maximum. Visibly darker blacks, and
  the benefit grows the faster the mode. Costs nothing in time: voltage sets how
  hard a pixel is driven, not how long.
- **Above +/-17 V is out of spec, and there is nothing there.** Frame rates do
  not change and the extra contrast is marginal. Treat the datasheet maximum as
  the ceiling — `volts` refuses anything outside the datasheet range rather than
  writing it.

Voltage is not persistent — the board reverts to stock on reset.

## Native greyscale

`gstill [RESET] [STEP]` takes two bitplanes from the host: `0x24` carries the LOW
bit and `0x26` the HIGH bit, which is the opposite of the obvious assumption.
Codes run 0 = black to 3 = white.

Grey is a **full repaint with a common reset** — every pixel is driven to black
before being pulled back toward white, because that is what makes the levels
absolute. So it flashes once per update, which is fine for a still and
unwatchable for video. That is why it is a separate command rather than a mode of
`stream`.

Step sizing matters more than it looks. The defaults inherited from the `gray`
demo (20/10) were calibrated at 15 V; at 17 V a full black-to-white transition
takes only ~12 frames, so codes 1, 2 and 3 all saturated to white and the image
came out nearly blank. **`--greset 12 --gstep 4`** sizes `3 x step` to exactly one
transition and restores the ramp.

## Known hardware fault

Two adjacent gate lines on this panel switch symmetrically slowly (~500 ms) and
only reach 80-90% black. Reproduced on the vendor waveform, on both SPI
transports, at maximum in-spec VGH. Concluded defective; replaced under
warranty.

The reason this is written down: for most of a session those two lines looked
like a waveform problem, and every waveform explanation for them was wrong.
A fault that survives the vendor waveform, both SPI transports and every
voltage in spec is hardware. Stop tuning and check the glass.

## Build

    python -m platformio run -d .
    python -m esptool --chip esp32s3 -p COM19 -b 460800 \
        --before default-reset --after hard-reset write-flash \
        --flash-mode dio --flash-freq 80m --flash-size 8MB \
        0x0 .pio/build/xiao426/firmware.factory.bin

See [OPEN-WORK.md](../OPEN-WORK.md) for what is unfinished.
