# SSD1677 — 4.26", 800x480

Bench, video streamer and native 4-level grey stills.

**Vendor full refresh: 540 ms. Differential updates here run from 30 ms up, and
video plays at 12.4 fps on 384,000 pixels with no flashing between frames.**

| | |
|--|--|
| panel | GDEQ0426T82, 4.26" mono, 800x480, one SSD1677 |
| MCU | XIAO ESP32-S3 — **native USB** (TinyUSB CDC), 8 MB flash, 8 MB PSRAM |
| pins | RST 1, CS 2, BUSY 3, DC 4, SCK 7, MOSI 9 |
| serial | 115200 (230400 also works; 921600 does **not** on this module) |
| RAM | row-major, 1 = white, 48000 bytes per plane |

## Build and flash

```
python -m platformio run -d .
python -m esptool --chip esp32s3 -p COMx -b 460800 --before default-reset \
    --after hard-reset write-flash --flash-mode dio --flash-freq 80m \
    --flash-size 8MB 0x0 .pio/build/xiao426/firmware.factory.bin
```

USB flags matter: `ARDUINO_USB_MODE=1` and `CDC_ON_BOOT=1`. Setting
`USB_MODE=0` dual-stacks the hardware CDC with TinyUSB and Windows reports a
failed device descriptor. If that has already happened: hold BOOT, tap RESET,
retry. Do not open a serial monitor before flashing.

## Timing model

```
frame_ms = (drive + 1) x 9.92 + 36
```

`(drive+1) x 9.92` is the waveform. FR code `0x88` gives 9.92 ms/frame, the
fastest `calib` found; the OEM tables ship `0x22` at 39.6 ms — four times slower
for no visible benefit.

`36` is flat per-frame cost: ~20 ms of SPI (both planes, 96000 bytes at 40 MHz)
plus ~16 ms of delta-apply and USB handshaking. **Fitted to measurement, not
assumed** — frame time minus waveform came out 35.8 / 35.8 / 35.9 / 36.1 / 36.1
/ 36.9 across drives 12 / 9 / 8 / 7 / 5 / 2.

SPI is unavoidable per frame because this controller has **no plane-copy
command**, so the old plane must be rewritten every time even when it is simply
the previous new plane.

| drive | 20 MHz | 40 MHz | | drive | 20 MHz | 40 MHz |
|-------|-------:|-------:|-|-------|-------:|-------:|
| 1 | — | 17.44 | | 7 | 7.68 | 8.66 |
| 2 | 12.42 | 15.00 | | 8 | 7.16 | 7.99 |
| 4 | 9.92 | 11.64 | | 10 | 6.27 | — |
| 5 | 9.04 | 10.46 | | 12 | — | 6.07 |

The 40 MHz gain grows as drive falls — 12% at drive 7, 21% at drive 2 — because
SPI is a fixed cost and therefore a larger share of a short waveform.

## The one SPI rule

The part is specced at 20 MHz and runs clean at 40 (80/2) on this breakout, with
jumper leads. **Set the clock at boot and never change it at runtime.**
Rebuilding the SPI device while the panel is live wedges the controller:
transfers still complete, it stops acting on commands, `busy_ms` reads 0, and
only a board reset clears it. 26.7 and 40 both wedged when switched into at
runtime and both are stable as the only speed the chip ever sees. `spi hw N` is
therefore clamped to 20; the boot value is the one that matters.

The ESP32 divides 80 MHz by an integer, so the choices are 20, 26.7 and 40 —
`spi hw 24` through `spi hw 32` all resolve to 26.7.

## Defaults

```
spi          40 MHz, once at boot
volts        VSH1 0x4B  VSL 0x3A   (+/-17 V, the datasheet maximum and the
             hard ceiling — `volts` rejects anything outside spec)
fr           0x88
invert LUT   true      (waveform polarity — NOT a host-side picture invert)
activate     0x0C      (Mode 2 differential, rails already up)
drive        2 for video, 12 for stills
settle       1
rim          1 px white, parked once
dither       cluster 8x8
tone         video gamma 1.30 contrast 1.40
grey stills  --greset 12 --gstep 4  (15 V tables saturate at 17 V)
```

Two things not to port from the SSD1683 sibling: on this part **bit 3 of `0x22`
selects Mode 1 vs Mode 2**, not 3-colour vs black-and-white; and the transition
LUT mapping is **inverted** relative to it. The inherited mapping drove the
moving object white and its trail black, which reads exactly like a picture-level
inversion bug and is not one.

`0xD7` is vendor Mode 1 full refresh **with a LUT reload** — use it to deghost,
then re-arm.

## Host tools

```
tools/play.py COMx frames/              # video, drive auto-selected
tools/play.py COMx image.png            # still, 1-bit dithered
tools/play.py COMx image.png --gray     # still, native 4-level grey
tools/play.py COMx frames --no-pace     # free-run, for benchmarking
tools/play.py COMx frames --sink        # measure the link with no panel
tools/mirror.py COMx --select           # mirror a live screen region
tools/spi_sweep.py COMx                 # SPI clock vs whether it refreshes
tools/analyze_compress.py frames/       # compare encodings offline
../../tools/webcam.py COMx              # live camera, multi-panel host
../../tools/serial_lab.py COMx out.log "cmd" ...
../../tools/peek.py COMx "status"
```

Playback is paced to real time by default. Without a clock the host ships frames
as fast as the panel accepts them, so a mode faster than the source needs plays
sped up — drive 2 at 14.9 fps against a 12.5 fps requirement runs 19% quick.
A mode slower than the requirement simply falls behind, and this tool does not
pretend otherwise.

## Native greyscale

`gstill [RESET] [STEP]` takes two bitplanes: `0x24` carries the **low** bit and
`0x26` the **high** bit, which is the opposite of the obvious assumption. Codes
run 0 = black to 3 = white.

It is a full repaint with a common reset, so it flashes once per update. Fine
for a still, unwatchable for video, which is why it is a separate command rather
than a mode of `stream`. The flash-free design is in
[docs/PICTURE.md](../../docs/PICTURE.md) and is not built.

## Note on the bench unit

Two adjacent gate lines on the panel this was measured on switch symmetrically
slowly (~500 ms) and only reach 80–90% black. Reproduced on the vendor waveform,
on both SPI transports, at the maximum in-spec VGH: defective glass, not a
software problem.
Do not try to fix it in software, and do not treat those two lines as evidence
about a waveform.

The reason it is written down: for most of a session those two lines looked
like a waveform problem, and every waveform explanation for them was wrong. A
fault that survives the vendor waveform, both SPI transports and every in-spec
voltage is hardware.

## Also in here

`firmware/main.cpp` includes a serial-driven falling-block game (`tetris`) as a
live-content test case. It is a demo, not the streamer — it exists because
solid-shape content and dithered photographic content need completely different
drive counts, and having both on one device makes that easy to demonstrate.
