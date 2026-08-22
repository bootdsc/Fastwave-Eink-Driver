# Dropping the fast waveform into a new app

The short version, for when you point me at this project and say "use this
display method". Read `SSD1683-FAST-WAVEFORM.md` for *why*; this file is *what to
copy*.

Target: CrowPanel 5.79", dual SSD1683, ESP32-S3, bit-bang SPI.

---

## Files to copy

```
waveform.h     waveform.cpp     <- the LUT builder, self-contained
firmware/EPD_Init.h     firmware/EPD_Init.cpp     <- panel init, RAM planes, activate
firmware/spi.h          firmware/spi.cpp          <- bit-bang + optional HW SPI
firmware/EPD.h          firmware/EPD.cpp          <- Paint/text/shapes (optional)
firmware/EPDfont.h                                <- fonts (only with EPD.cpp)
```

`waveform.*` depends only on `EPD_Init.*` and `spi.*`. Nothing depends on the lab
app itself.

Add to `platformio.ini`:

```ini
build_src_filter =
    +<your_app.cpp>
    +<waveform.cpp>
    +<EPD.cpp>
    +<EPD_Init.cpp>
    +<spi.cpp>
```

---

## Minimal integration

```c
#include "EPD_Init.h"
#include "waveform.h"

static uint8_t fb[EPD_H * FB_WBYTES];      // current frame
static uint8_t fb_prev[EPD_H * FB_WBYTES]; // previous frame (true differential)
static uint8_t ws[WAVE_BYTES];
static WaveParams wp;

void display_begin(void) {
  pinMode(7, OUTPUT); digitalWrite(7, HIGH); delay(20);  // panel power
  // Set the clock ONCE, here. Do not change it at runtime — see the README.
  EPD_SetHwSpiHz(20000000);        // datasheet max; 101 ms -> 39 ms per frame
  EPD_SetSpiPath(EPD_SPI_HW);
  EPD_GPIOInit();
  EPD_FastMode1Init_Ex(0xB1);              // fake 100 C -> hottest OTP bank

  EPD_Display_Clear();                      // one GC, at boot only
  EPD_Update();
  EPD_LoadWhiteBothPlanes();
  memset(fb_prev, 0xFF, sizeof(fb_prev));

  wave_defaults(&wp);                       // FR 0x05, VCOM 0x08, mask = VCOM
  wp.reset_tp  = 0;                         // MUST be 0 when trail chasing
  wp.drive_tp  = 10;                        // <- the speed dial. 10 = 89 ms
  wp.settle_tp = 1;
  // Source rails at the datasheet maximum: ~13% more drive for ZERO extra time.
  wp.vsh1 = 0x4B; wp.vsl = 0x3A;
  wp.volt_mask = WV_V_VCOM | WV_V_SRC;      // never add EOPT: +17 ms and a flash
  wave_build(ws, &wp);
  wave_load(ws, /*dual=*/true, wp.volt_mask);   // BOTH chips
}

void display_frame(void) {
  EPD_RestoreFullWindows();
  EPD_Display(fb);                          // NEW plane
  EPD_Display_OldPlane_Ex(fb_prev, false);  // OLD plane = previous frame

  uint32_t busy = 0;
  // Raises the rails if they are down and leaves them up afterwards, so the ramp
  // is paid once rather than per frame. DO NOT keep your own rails_up flag: only
  // the driver sees every reset, and a stale flag turns every 0x0C into a silent
  // no-op with busy = 0 as the only symptom.
  EPD_UpdateSeqPlain_Timed(0x0C, &busy);    // DISPLAY only — no ramp, no fixed cost

  memcpy(fb_prev, fb, sizeof(fb_prev));
}

void display_idle(void) {                   // when updates stop
  if (EPD_AnalogIsOn()) EPD_AnalogPower(false);
}
```

`busy` will be `(reset_tp + drive_tp + settle_tp) * 8.11 ms`, exactly. You can
compute it up front with `wave_frames(ws) * wave_fr_period_us(ws[224])`.

---

## Rules that are not optional

1. **Write the LUT to BOTH cascade chips** — `wave_load(..., dual = true)`.
   Master-only leaves the two halves on different waveforms.
2. **Anything that runs the OTP path drops your LUT.** `0xDC`, `0xF7`, `0xFF`,
   `0x9x`, `0xBx`, `EPD_Update()`, `EPD_PartUpdate()`, `EPD_Display_Clear()`,
   and any re-init. **Re-arm with `wave_load()` afterwards.** Every full clear or
   re-seed in this codebase is followed by a re-arm for exactly this reason.
3. **Never send `0x0C` / `0x8C` while the OTP waveform is loaded.** BUSY never
   releases — 8-second timeouts, then the board resets.
4. **Never write the voltage trailer as a block.** Use the mask. `EOPT = 0x22`
   costs +17 ms per update and flashes.
5. **Never OTP burn (`0x30`).** Everything here is volatile register state and a
   power cycle undoes all of it.
6. `VCOM` is only valid in `0x08`..`0x78`. `0x00` is undefined.
7. **Never keep your own analog-rail flag.** Use `EPD_AnalogIsOn()`. An app-owned
   copy goes stale the instant anything pulses RST — a re-init, a busy-timeout
   recovery — and from then on every bare `0x0C` silently does nothing, with
   `busy_us = 0` the only symptom. It looks exactly like a dead SPI path.
8. **Set the SPI clock once, at boot.** Rebuilding the SPI device while the panel
   is live wedges the SSD1677 sibling outright, and there is no reason to think
   this part is more forgiving. 20 MHz is the datasheet maximum here and 40 MHz
   would save ~5 ms of a ~130 ms frame — not worth it.

---

## Picking the speed dial

`wp.drive_tp` is the only number you normally change.

With `spi hw 20` (what the snippet above sets up):

| drive | busy | + HW SPI | frame | measured fps |
|-------|------|----------|-------|--------------|
| 10 | 89 ms | 39 ms | 129 ms | 6.8 |
| 5 | 48 ms | 39 ms | 88 ms | 9.5 |

Measured fps is wall clock and sits below `1000 / frame` by the render and
bookkeeping outside the timed region. Older bit-bang figures, for reference:

| drive | busy | + bit-bang SPI | wall clock |
|-------|------|----------------|------------|
| 16 | 154 ms | 86 ms | 240 ms |
| 12 | 121 ms | 86 ms | 207 ms |
| 8 | 89 ms | 86 ms | 175 ms |
| 4 | 56 ms | 86 ms | 142 ms |

Lower `drive` costs contrast: the to-white transition stops completing and leaves
grey trails where content has moved. Where that becomes unacceptable is a
judgement call on the actual content — run the lab and look.

Below about `drive 8` most of the frame is SPI, not waveform, so further
reductions buy little. See §8 of the reference doc.

---

## Ghost management

Ghosting still accumulates — it is a differential partial waveform, that is
inherent. Two options, neither needing a full-screen GC flash:

- **False-diff repaint:** write the image to NEW and its inverse to OLD, then
  fire. Every pixel reads as changed and gets a full transition.
  `EPD_Display_OldPlane_Ex(fb, true)`. One pass at the vendor waveform is 336 ms;
  at `drive 16` it is 154 ms.
- **Periodic scrub:** the above every N frames. At `drive 8` and N = 24 that is
  roughly +7 ms/frame amortised.

After any scrub, restore `OLD = image` (`EPD_Display_OldPlane_Ex(fb, false)`) or
the next differential frame diffs against an inverted plane and inverts.

---

## Bringing up a panel that is not this one

Do these two before anything else — both are non-destructive and either could
save days:

1. `calib` — all-VSS waveform, sweep frame count per FR code. Gives frame period
   and fixed cost with zero risk to the image.
2. `seqscan` — same waveform, vary only the `0x22` byte. Finds the cheapest
   activate sequence.

If the frame period does not move with FR, byte 224 is being ignored and only
frame *count* will help.
