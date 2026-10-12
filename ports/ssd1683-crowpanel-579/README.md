# SSD1683 — 5.79", 792x272, two cascaded controllers

The waveform lab. This is where the method in
[docs/WAVEFORM-LAW.md](../../docs/WAVEFORM-LAW.md) was worked out, and it is the
most instrumented of the three ports.

**Vendor partial: 336 ms. This: 89 ms at MODE 10, 48 ms at MODE 5.**

| | |
|--|--|
| panel | 5.79" mono, 792x272 visible |
| controllers | **two** SSD1683, cascaded, each 400x272; RAM is 800 wide, so there is an 8-pixel seam |
| MCU | ESP32-S3, USB-C via a UART bridge (**not** native USB — around 200 kB/s, and no baud setting changes that) |
| pins | SCK 12, MOSI 11, RST 47, DC 46, CS 45, BUSY 48, panel power GPIO 7 **must be driven high** |
| serial | 115200 |
| framebuffer | column-major, so each controller's half is one contiguous burst |

## Build and flash

```
python -m platformio run -d .
python -m esptool --chip esp32s3 -p COMx -b 460800 --before default-reset \
    --after hard-reset write-flash --flash-mode dio --flash-freq 80m \
    --flash-size 8MB 0x0 .pio/build/wavelab/firmware.factory.bin
```

It boots to a short "Fastwave" splash, clears to white and waits for Ink Flow.
`run` starts the bench: one moving disc with the mode, waveform time, total and
frame rate in the corner.

## Your own boot message

The splash is the BOOT SPLASH block at the top of
[firmware/wavelab.cpp](firmware/wavelab.cpp). Change `BOOT_SPLASH_TEXT` to put your
project's name on the panel at power-on, `BOOT_SPLASH_MS` for how long it stays up,
or set the text to `""` to boot straight to white. Size and orientation sit in the
same block.

## Defaults

```
spi          20 MHz hardware, set once in setup()
volts        VSH1 0x4B  VSL 0x3A   (+/-17 V, datasheet max; vsh1/vsh2/vsl
             refuse anything above it)
vcom         0x08                  (-0.2 V — stops the panel creeping dark)
fr           0x05                  (125 Hz, 8.11 ms/frame)
activate     0x0C after 0xC0       (rails up once, then display only)
trail        4                     (reset phase MUST then be 0)
rim          1 px, parked once
```

`volt_mask` is `VCOM|SRC`. **EOPT must stay out of it here** — on this panel it
costs +17 ms and a visible flash, which is the opposite of its effect on the
1.54". Same register, different answer.

## Measured

With `spi hw 20`, `reset 0 / settle 1`, FR `0x05`, activate `0x0C`, trail 4:

| MODE | waveform | SPI | total | wall-clock fps | vs vendor |
|------|---------:|----:|------:|---------------:|----------:|
| 10 | 89 ms | 39 ms | 129 ms | 6.8 | 2.9x |
| 5 | 48 ms | 39 ms | 88 ms | 9.5 | 4.2x |
| 2 | 24 ms | 40 ms | 65 ms | — | 5.8x |
| 1 | 16 ms | 40 ms | 56 ms | — | 6.8x |

**MODE is the drive frame count and nothing else changes between modes** — same
LUT shape, same activate byte, same voltages, same frame rate. That is
deliberate, so a mode comparison is a comparison of one variable.

Wall-clock sits below `1000/total` by ~18 ms of render, trail bookkeeping and
serial output that falls outside the timed region. Both are reported because the
gap is real work.

**SPI is the bottleneck now.** See the reference doc for the four-stage journey
from 101 ms of bit-bang to 40 ms of strided DMA, and the ~17 ms of per-frame
driver overhead that is still unexplained.

## Serial

`help` lists everything. The ones that matter:

```
calib          frame period per FR code      — cannot change the glass
seqscan        fixed cost per activate byte  — cannot change the glass
wave           current params + decoded LUT
drive N        frames of target-polarity drive (the speed dial)
reset N        opposite-polarity kick frames (0 when trail chasing)
settle N       VSS hold frames
fr N           1=25 2=50 3=75 4=100 5=125 Hz  (9..12 = the half-steps)
volt MASK      1=EOPT 2=VGH 4=source 8=VCOM
vcom X         0x08 = -0.2 V .. 0x78 = -3 V   (0x00 is INVALID)
vsh1 X vsl X   source drive; 0x4B / 0x3A = +/-17 V
trail N        trail-chase depth
probe B L T    drive one LUT block — identifies the transition mapping
```

Buttons: wheel up = MODE +1, wheel down or click = MODE −1, right = run, left =
stop and drop the rails. Every mode change clears and reseeds first, so you
judge that waveform alone. The buttons deliberately cannot reach the other bench
modes — landing in one mid-comparison silently swaps the waveform out, which is
indistinguishable from the waveform misbehaving.

Attach with `../../tools/serial_lab.py`, not a plain terminal: a default connect
resets the board and destroys whatever run is in progress.

## Read next

- [docs/panels/SSD1683-crowpanel-5.79.md](../../docs/panels/SSD1683-crowpanel-5.79.md)
  — the full measurement record: timing law, LUT format, register encodings.
- [docs/INTEGRATION-ssd1683.md](../../docs/INTEGRATION-ssd1683.md) — dropping
  this driver into an application.
- [docs/panels/SSD1683-video-player.md](../../docs/panels/SSD1683-video-player.md)
  — the video path, mode by mode.

## Known and unfixed

- Windowed transfer measures a real win and is **off and not trusted**. See
  [docs/WINDOWED-TRANSFER.md](../../docs/WINDOWED-TRANSFER.md).
- One BUSY timeout on the first update after a clear-seed-arm handoff. Recovers
  on its own; root cause not chased.
- A vendor full refresh can glitch the button input pins. Mute inputs around it.
