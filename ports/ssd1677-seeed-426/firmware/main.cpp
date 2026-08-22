// Seeed 4.26" 800x480 (GDEQ0426T82 / SSD1677) on XIAO ESP32-S3 — bring-up bench.
//
// This is the first firmware for the board: prove the panel, then measure it.
// It deliberately does NOT port everything from the 5.79 bench. The order that
// worked there was: get a clean image, establish the timing law, then optimise —
// and the expensive mistakes all came from optimising before measuring.
//
// Serial 115200:
//   help | status | clear | white | black | run | stop | step
//   calib      frame period vs FR code   — cannot change the glass
//   seqscan    fixed cost per 0x22 byte  — cannot change the glass
//   drive N | settle N | fr N | rp N | pol 0|1
//   wave       show the built LUT
//   custom 0|1 use the custom waveform (0 = vendor partial 0xFC)
//   seq X      force the activate byte (0x0C fast path, 0xFC vendor)
//   spi hw N | spi bitbang
//
// Build: python -m platformio run -d .

#include <Arduino.h>
#include <string.h>

#include "board.h"
#include "spi.h"
#include "ssd1677.h"
#include "wave1677.h"

// ---- framebuffers -----------------------------------------------------------
// 48000 bytes each. Internal RAM: the XIAO S3 has 512 KB, and these are read
// byte-by-byte during the transfer where PSRAM stalls would show up.
static uint8_t g_fb[EPD_PLANE_BYTES];
static uint8_t g_prev[EPD_PLANE_BYTES];
// Low bitplane, used only in 4-level grey mode. In grey mode 0x24 carries the
// high bit of each pixel's 2-bit code and 0x26 the low bit — the planes stop
// meaning new/old for the duration.
static uint8_t g_fb_lsb[EPD_PLANE_BYTES];

// Drive values to step through, mirroring the 1/2/5/10 wheel on the 5.79 bench.
// This board has no buttons, so it advances on a timer instead.
static const uint8_t kCycle[] = {12, 5, 2, 1};
static uint32_t g_cycle_ms = 0;  // 0 = off
static uint32_t g_cycle_at = 0;
static uint8_t g_cycle_i = 0;

// ---- state ------------------------------------------------------------------
static Wave1677 g_wp;
static uint8_t g_lut[WAVE1677_BYTES];
static bool g_custom = false;   // false = vendor partial, true = our LUT
static uint8_t g_seq = 0;       // 0 = pick automatically
static bool g_running = false;
static bool g_step_once = false;
static uint32_t g_frame = 0;
static uint32_t g_last_spi_us = 0, g_last_busy_us = 0, g_last_tot_us = 0;

// ---- wall-clock fps --------------------------------------------------------
// Kept in sync with the 5.79 bench, which shows the same figure on its panel (it
// has a font; this one deliberately does not).
//
// Measured from the CLOCK, not derived from tot_ms, so it includes everything
// outside the timed region — render, the g_prev memcpy, and the serial line
// itself. On the 5.79 that gap turned out to be 18 ms a frame, 10 ms of which was
// the per-frame RESULT printf, so the number worth reporting is the one that
// contains it. Recomputed once a second and on nothing else: frame time depends on
// what gets logged, so anything downstream of the readout must not decide when the
// readout updates.
static char g_fps_num[8] = "";
static uint32_t g_fps_t0 = 0;
static uint32_t g_fps_frames = 0;

static void fps_reset(void)
{
  g_fps_t0 = 0;
  g_fps_frames = 0;
  g_fps_num[0] = '\0';  // blank beats stale: a rate from the previous regime is a lie
}

static void fps_tick(void)
{
  uint32_t now = millis();
  if (!g_fps_t0) {  // first frame of a window carries no interval yet
    g_fps_t0 = now;
    g_fps_frames = 0;
    return;
  }
  g_fps_frames++;
  uint32_t dt = now - g_fps_t0;
  if (dt < 1000) return;
  uint32_t f10 = (g_fps_frames * 10000UL + dt / 2) / dt;
  snprintf(g_fps_num, sizeof(g_fps_num), "%lu.%lu", (unsigned long)(f10 / 10),
           (unsigned long)(f10 % 10));
  g_fps_t0 = now;
  g_fps_frames = 0;
}

static char g_line[96];
static uint8_t g_len = 0;

// Declared up front: the graphics and demo code below both need them, and they
// are defined further down next to the waveform handling they belong with.
static uint32_t predicted_busy_ms(void);
static void arm_wave(void);

// ---- host frame streaming ---------------------------------------------------
// Two buffers, one being filled from USB while the other is on its way to the
// panel. See the `stream` command for why this is worth a task and a core.
//
// 12288 bytes per handshake: it only has to fit inside the CDC receive queue,
// and chunk size turned out not to affect throughput at all — 4096, 12288 and a
// whole 48000-byte frame all measured 4.9 fps, which is what identified the link
// itself, rather than round-trip latency, as the limit.
#define STREAM_CHUNK 12288

static uint8_t *g_sbuf[2];
static SemaphoreHandle_t g_sem_free = NULL, g_sem_ready = NULL;
static volatile bool g_srx_run = false;
static volatile bool g_srx_fail = false;

// Frames arrive as a DELTA against the previous frame, which is what lets the
// link keep up with the panel.
//
// Wire format, per frame:
//   6000 bytes  bitmap, one bit per byte of the frame, set = that byte changed
//   N bytes     the new value of each changed byte, in order
//
// N is not transmitted — both ends derive it by counting bits, so there is no
// length field to get out of step. Measured on the clip: 25% of bytes change per
// frame, giving 17853 bytes against 48000, i.e. 2.69x.
//
// This only works because the dither is a fixed 4x4 ordered matrix: a static
// area produces byte-identical output every frame, so it drops out of the delta
// entirely. Error-diffusion dithering would perturb every pixel each frame and
// leave nothing to remove.
static uint8_t g_bmp[EPD_PLANE_BYTES / 8];
static uint8_t g_lit[EPD_PLANE_BYTES];

// Pull exactly `total` bytes, asking for them a chunk at a time.
static bool stream_read_exact(uint8_t *dst, uint32_t total)
{
  for (uint32_t off = 0; off < total; off += STREAM_CHUNK) {
    uint32_t want = total - off;
    if (want > STREAM_CHUNK) want = STREAM_CHUNK;
    Serial.write((uint8_t)0x06);
    // Bulk read, NOT Serial.readBytes(): that is Stream::readBytes, which calls
    // read() once per byte and so performs one queue operation per byte.
    uint32_t have = 0, tw = millis();
    while (have < want && millis() - tw < 4000) {
      int n = Serial.read(dst + off + have, want - have);
      if (n > 0) { have += (uint32_t)n; tw = millis(); }
      // delay(1), NOT delay(0). vTaskDelay(0) yields only to tasks of equal or
      // higher priority, so this task — priority 2 on core 0 — starves the core's
      // IDLE task and the watchdog panics the board after ~5 s. That stayed
      // hidden while the panel was the bottleneck, because both buffers filled
      // and this task blocked on g_sem_free often enough to let IDLE run. Adding
      // host-side pacing made the HOST the bottleneck, so it spins through most
      // of every frame interval instead, and the board rebooted mid-clip.
      //
      // The tick costs nothing measurable: when this was delay(1) originally the
      // link measured the same 4.9 fps it did as delay(0).
      else delay(1);
    }
    if (have != want) {
      Serial.printf("SHORT got=%u want=%u off=%u\n", (unsigned)have,
                    (unsigned)want, (unsigned)off);
      return false;  // short read = host finished, or stalled
    }
  }
  return true;
}

// Rebuild a frame into `dst` from `base` plus the delta on the wire. `base` is
// the other ping-pong buffer — the consumer only ever reads it, so taking it as
// the reconstruction source needs no extra 48 KB and no extra synchronisation.
static bool stream_recv(uint8_t *dst, const uint8_t *base)
{
  if (!stream_read_exact(g_bmp, sizeof(g_bmp))) return false;

  uint32_t n = 0;
  for (uint32_t i = 0; i < sizeof(g_bmp); i++) n += __builtin_popcount(g_bmp[i]);
  if (n && !stream_read_exact(g_lit, n)) return false;

  memcpy(dst, base, EPD_PLANE_BYTES);
  uint32_t k = 0;
  for (uint32_t i = 0; i < sizeof(g_bmp); i++) {
    uint8_t m = g_bmp[i];
    if (!m) continue;  // the common case: skip eight untouched bytes at once
    uint32_t at = i * 8;
    for (uint8_t b = 0; b < 8; b++)
      if (m & (0x80 >> b)) dst[at + b] = g_lit[k++];
  }
  return true;
}

static void stream_rx_task(void *arg)
{
  (void)arg;
  for (uint32_t i = 0;; i++) {
    if (xSemaphoreTake(g_sem_free, pdMS_TO_TICKS(200)) != pdTRUE) {
      if (!g_srx_run) break;
      i--;  // nothing was consumed, so do not advance the buffer index
      continue;
    }
    if (!g_srx_run) break;
    if (!stream_recv(g_sbuf[i & 1], g_sbuf[(i + 1) & 1])) {
      g_srx_fail = true;
      xSemaphoreGive(g_sem_ready);  // wake the consumer so it can stop too
      break;
    }
    xSemaphoreGive(g_sem_ready);
  }
  vTaskDelete(NULL);
}

// ---- tiny graphics ----------------------------------------------------------
// The framebuffer stays in the panel's native layout — row-major, 800 px wide,
// MSB = leftmost, 1 = white — so a full-panel write remains one contiguous run
// with no conversion. Rotation happens here, in the pixel setter, and nowhere
// else.
//
// Logical canvas is PORTRAIT 480x800, rotated 90 degrees CLOCKWISE onto the
// landscape panel: logical (x, y) -> native (EPD_W-1-y, x).
#define LCD_W 480
#define LCD_H 800

// When true, px() maintains BOTH bitplanes so ordinary black/white drawing works
// in grey mode. Without it the labels only touched the high plane, leaving the
// low plane set — so "black" text rendered as a light grey code and was almost
// unreadable.
static bool g_draw_gray = false;

static inline void px(int x, int y, bool black)
{
  if (x < 0 || y < 0 || x >= LCD_W || y >= LCD_H) return;
  int nx = EPD_W - 1 - y;
  int ny = x;
  uint32_t idx = (uint32_t)ny * EPD_WBYTES + (uint32_t)(nx >> 3);
  uint8_t bit = (uint8_t)(0x80 >> (nx & 7));
  if (black) g_fb[idx] &= (uint8_t)~bit;
  else g_fb[idx] |= bit;
  if (g_draw_gray) {
    if (black) g_fb_lsb[idx] &= (uint8_t)~bit;
    else g_fb_lsb[idx] |= bit;
  }
}

static void fb_fill(uint8_t v) { memset(g_fb, v, sizeof(g_fb)); }

static void fill_rect(int x0, int y0, int w, int h, bool black)
{
  for (int y = y0; y < y0 + h; y++)
    for (int x = x0; x < x0 + w; x++) px(x, y, black);
}

static void fill_disc(int cx, int cy, int r, bool black)
{
  for (int dy = -r; dy <= r; dy++) {
    int span = (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f);
    for (int dx = -span; dx <= span; dx++) px(cx + dx, cy + dy, black);
  }
}

// 8x8 digits, scaled up when drawn. Only 0-9 — enough to show the mode and the
// millisecond figure, and it avoids dragging in a 100 KB font for bring-up.
static const uint8_t kDigits[10][8] = {
    {0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00},  // 0
    {0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00},  // 1
    {0x3C, 0x66, 0x06, 0x0C, 0x30, 0x60, 0x7E, 0x00},  // 2
    {0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00},  // 3
    {0x0C, 0x1C, 0x3C, 0x6C, 0x7E, 0x0C, 0x0C, 0x00},  // 4
    {0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00},  // 5
    {0x1C, 0x30, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00},  // 6
    {0x7E, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00},  // 7
    {0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00},  // 8
    {0x3C, 0x66, 0x66, 0x3E, 0x06, 0x0C, 0x38, 0x00},  // 9
};

static void draw_digit(int x, int y, uint8_t d, int scale)
{
  if (d > 9) return;
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 8; c++)
      if (kDigits[d][r] & (0x80 >> c))
        fill_rect(x + c * scale, y + r * scale, scale, scale, true);
}

static void draw_number(int x, int y, uint32_t n, int scale)
{
  char buf[12];
  int len = snprintf(buf, sizeof(buf), "%lu", (unsigned long)n);
  for (int i = 0; i < len; i++)
    draw_digit(x + i * 9 * scale, y, (uint8_t)(buf[i] - '0'), scale);
}

// ---- 4-level grayscale ------------------------------------------------------
// code 0..3, written across the two bitplanes. Which code lands on which shade
// is panel convention rather than something the datasheet fixes, so the demo
// draws all four in order and labels them — the glass decides the mapping.
static inline void px_gray(int x, int y, uint8_t code)
{
  if (x < 0 || y < 0 || x >= LCD_W || y >= LCD_H) return;
  int nx = EPD_W - 1 - y;
  int ny = x;
  uint32_t idx = (uint32_t)ny * EPD_WBYTES + (uint32_t)(nx >> 3);
  uint8_t bit = (uint8_t)(0x80 >> (nx & 7));
  // 0x24 (g_fb) is the LOW bit and 0x26 (g_fb_lsb) is the HIGH bit — the
  // opposite of the obvious assumption. Measured on glass: with the planes the
  // other way round, code 1 came out LIGHTER than code 2, which is exactly the
  // drive assigned to LUT2 and LUT1 respectively. Codes 0 and 3 are symmetric,
  // so they looked correct and concealed it.
  if (code & 1) g_fb[idx] |= bit; else g_fb[idx] &= (uint8_t)~bit;
  if (code & 2) g_fb_lsb[idx] |= bit; else g_fb_lsb[idx] &= (uint8_t)~bit;
}

static void gray_disc(int cx, int cy, int r, uint8_t code)
{
  for (int dy = -r; dy <= r; dy++) {
    int span = (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f);
    for (int dx = -span; dx <= span; dx++) px_gray(cx + dx, cy + dy, code);
  }
}

// Two codes on a checkerboard. The panel only has four true levels; alternating
// pixels between two of them reads as a shade in between at normal viewing
// distance, which is how you get more apparent levels than the hardware has.
static void gray_disc_dither(int cx, int cy, int r, uint8_t a, uint8_t b)
{
  for (int dy = -r; dy <= r; dy++) {
    int span = (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f);
    for (int dx = -span; dx <= span; dx++) {
      int x = cx + dx, y = cy + dy;
      px_gray(x, y, ((x + y) & 1) ? a : b);
    }
  }
}

// ---- the measured palette ---------------------------------------------------
//
// TEN distinct shades from a 2-bit panel, ordered dark to light. Read off the
// glass 2026-08-08 with `ramp 24 6` — every entry was visually distinct and the
// ordering is monotonic:
//
//   0 -> 10 -> 20 -> 1 -> 30 -> 21 -> 2 -> 31 -> 32 -> 3
//
// Four of those are native levels; the other six are checkerboard dithers of two
// native codes. Note the ordering is NOT what you would predict by averaging the
// codes: 30 (white+black) sits between 1 and 21 rather than in the middle,
// because ink coverage is not perceptually linear. This table is measurement,
// not arithmetic — do not "correct" it to look tidier.
#define GRAY_LEVELS 10
static const uint8_t kPalette[GRAY_LEVELS][2] = {
    {0, 0},  // 0  black
    {1, 0},  // 1  dark grey
    {2, 0},  // 2
    {1, 1},  // 3
    {3, 0},  // 4
    {2, 1},  // 5
    {2, 2},  // 6
    {3, 1},  // 7
    {3, 2},  // 8  lightest grey
    {3, 3},  // 9  white
};

// Shade `level` (0..9) at a pixel, dithering where the palette entry needs it.
static inline void px_shade(int x, int y, uint8_t level)
{
  if (level >= GRAY_LEVELS) level = GRAY_LEVELS - 1;
  uint8_t a = kPalette[level][0], b = kPalette[level][1];
  px_gray(x, y, (a == b) ? a : (((x + y) & 1) ? a : b));
}

// Full palette: the four native codes plus every dither pair, as horizontal
// bands. Confirmed on glass 2026-08-08 with `gray 24 6`:
//   code 0 = black, 1 = dark grey, 2 = light grey, 3 = white
// and dithering adjacent codes gives usable intermediates — 3/2 in particular
// reads as a clean very-light grey. Dithering DISTANT codes does not work as
// well: 3/0 (50% black-white checkerboard) reads about as dark as code 1 rather
// than as a midpoint, because ink coverage is not perceptually linear.
static void gray_ramp(uint8_t reset_tp, uint8_t step_tp)
{
  g_running = false;
  g_draw_gray = true;
  memset(g_fb, 0xFF, sizeof(g_fb));
  memset(g_fb_lsb, 0xFF, sizeof(g_fb_lsb));

  // a == b means a solid native level; a != b means dither the pair.
  static const uint8_t kPairs[][2] = {
      {0, 0}, {1, 0}, {1, 1}, {2, 1}, {2, 2}, {3, 2}, {3, 3}, {3, 1}, {3, 0}, {2, 0},
  };
  const int n = (int)(sizeof(kPairs) / sizeof(kPairs[0]));
  const int bh = LCD_H / n;

  for (int i = 0; i < n; i++) {
    uint8_t a = kPairs[i][0], b = kPairs[i][1];
    for (int y = i * bh; y < (i + 1) * bh - 4; y++)
      for (int x = 40; x < LCD_W - 40; x++)
        px_gray(x, y, (a == b) ? a : (((x + y) & 1) ? a : b));
    // Label each band with its two codes, in black, on the left margin.
    draw_number(6, i * bh + 8, a, 3);
    if (a != b) draw_number(6, i * bh + 34, b, 3);
  }

  wave1677_build_gray(g_lut, reset_tp, step_tp, g_wp.fr, g_wp.invert);
  EPD_WriteLut(g_lut);
  EPD_WriteVoltages(g_wp.vgh, g_wp.vsh1, g_wp.vsh2, g_wp.vsl, g_wp.vcom);
  EPD_SetRamOption(RAM_NORMAL, RAM_NORMAL);
  EPD_WriteNew(g_fb);
  EPD_WriteOldMasked(g_fb_lsb, nullptr);
  uint32_t b = 0;
  EPD_Update(0xC7, &b);
  Serial.printf("RAMP %d bands, reset=%u step=%u busy_ms=%lu\n", n, (unsigned)reset_tp,
                (unsigned)step_tp, (unsigned long)(b / 1000UL));
  g_draw_gray = false;
  arm_wave();
}

// reset_tp / step_tp are arguments so the ramp can be reshaped over serial
// without reflashing — the four levels came out bunched at the light end on the
// first try, and finding the spacing is a sweep, not a calculation.
static void gray_demo(uint8_t reset_tp, uint8_t step_tp)
{
  g_running = false;
  g_draw_gray = true;  // keep both planes coherent for the labels
  memset(g_fb, 0xFF, sizeof(g_fb));
  memset(g_fb_lsb, 0xFF, sizeof(g_fb_lsb));

  const int r = 52;
  const int cx = 150;
  // Four native codes, top to bottom, each with its number beside it.
  for (uint8_t c = 0; c < 4; c++) {
    int cy = 90 + c * 125;
    gray_disc(cx, cy, r, c);
    draw_number(cx + 90, cy - 24, c, 6);
  }
  // Two dithered discs: 3/0 alternating is the widest possible fake mid-tone,
  // 3/2 a subtle one. If these read as extra shades, dithering multiplies the
  // four native levels into something usable for images.
  // The dithered pair also gets labelled — the first version numbered only the
  // four native discs, so the two most interesting ones were unidentifiable.
  gray_disc_dither(cx, 90 + 4 * 125, r, 3, 0);
  draw_number(cx + 90, 90 + 4 * 125 - 24, 30, 6);  // reads "30" = dither 3 with 0
  gray_disc_dither(cx, 90 + 5 * 125, r, 3, 2);
  draw_number(cx + 90, 90 + 5 * 125 - 24, 32, 6);  // reads "32" = dither 3 with 2

  // Grey is a full refresh, not a differential one: load the ramp, push both
  // bitplanes, and fire Mode 1. 0xC7 displays without reloading the factory LUT.
  wave1677_build_gray(g_lut, reset_tp, step_tp, g_wp.fr, g_wp.invert);
  EPD_WriteLut(g_lut);
  EPD_WriteVoltages(g_wp.vgh, g_wp.vsh1, g_wp.vsh2, g_wp.vsl, g_wp.vcom);
  EPD_SetRamOption(RAM_NORMAL, RAM_NORMAL);
  EPD_WriteNew(g_fb);                     // 0x24 = LOW bit
  EPD_WriteOldMasked(g_fb_lsb, nullptr);  // 0x26 = HIGH bit
  uint32_t b = 0;
  EPD_Update(0xC7, &b);
  Serial.printf("GRAY reset=%u step=%u frames=%u busy_ms=%lu — discs 0,1,2,3 then "
                "dither(3/0), dither(3/2)\n",
                (unsigned)reset_tp, (unsigned)step_tp,
                (unsigned)wave1677_frames(g_lut), (unsigned long)(b / 1000UL));
  g_draw_gray = false;
  arm_wave();  // put the b/w waveform back
}

// ---- animated greyscale -----------------------------------------------------
//
// Grey has no differential mode: both RAM planes are bitplanes, so there is no
// "previous image" to diff against and every frame is a full repaint. What CAN
// be removed is the analog ramp — the one-shot demo uses 0xC7, which powers the
// block down afterwards and pays ~220 ms for it. 0x04 is Display Mode 1 with no
// ramp and no power-down, exactly the same trick as 0x0C on the mono path.
//
// So an animated grey frame costs frames x period and nothing else, which at 24
// frames is ~240 ms rather than 637.
static bool g_gray_anim = false;
static uint8_t g_ganim_reset = 12, g_ganim_step = 4;

#define GDISCS 4
static int gx[GDISCS], gy[GDISCS], gsx[GDISCS], gsy[GDISCS];
// Spread across the measured palette so the discs are obviously different
// shades rather than four nearly-identical greys.
static const uint8_t kDiscLevel[GDISCS] = {0, 3, 6, 8};

static void gray_anim_init(void)
{
  for (int i = 0; i < GDISCS; i++) {
    gx[i] = 90 + i * 90;
    gy[i] = 120 + i * 150;
    gsx[i] = (i & 1) ? 1 : -1;
    gsy[i] = (i & 2) ? 1 : -1;
  }
}

static void gray_anim_frame(void)
{
  const int r = 46;
  for (int i = 0; i < GDISCS; i++) {
    gx[i] += gsx[i] * 26;
    gy[i] += gsy[i] * 34;
    if (gx[i] < r) { gx[i] = r; gsx[i] = 1; }
    if (gx[i] > LCD_W - r) { gx[i] = LCD_W - r; gsx[i] = -1; }
    if (gy[i] < r) { gy[i] = r; gsy[i] = 1; }
    if (gy[i] > LCD_H - r) { gy[i] = LCD_H - r; gsy[i] = -1; }
  }

  memset(g_fb, 0xFF, sizeof(g_fb));  // both planes set = whitest code
  memset(g_fb_lsb, 0xFF, sizeof(g_fb_lsb));
  for (int i = 0; i < GDISCS; i++)
    for (int dy = -r; dy <= r; dy++) {
      int span = (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f);
      for (int dx = -span; dx <= span; dx++)
        px_shade(gx[i] + dx, gy[i] + dy, kDiscLevel[i]);
    }

  uint32_t s0 = micros();
  EPD_SetRamOption(RAM_NORMAL, RAM_NORMAL);
  EPD_WriteNew(g_fb);
  EPD_WriteOldMasked(g_fb_lsb, nullptr);
  g_last_spi_us = micros() - s0;

  uint32_t busy = 0;
  EPD_Update(0x04, &busy);  // Mode 1 display, rails stay up
  g_last_busy_us = busy;
  g_last_tot_us = g_last_spi_us + busy;

  if ((g_frame & 7) == 0)
    Serial.printf("GFRAME f=%lu spi_ms=%lu busy_ms=%lu tot_ms=%lu\n",
                  (unsigned long)g_frame, (unsigned long)(g_last_spi_us / 1000UL),
                  (unsigned long)(busy / 1000UL), (unsigned long)(g_last_tot_us / 1000UL));
}

// ---- dithered mono ----------------------------------------------------------
//
// Apparent grey on the 1-bit differential path. Unlike the native 4-level mode
// this NEVER flashes: a differential update only drives pixels that changed, so
// there is no global reset and nothing to flash. Ordered 4x4 dithering gives 17
// apparent levels — more than the hardware's four — at the mono frame rate.
//
// The cost is spatial: detail below a 4x4 block turns into pattern. At 219 PPI
// that is a good trade, and it is how flash-free grey video is done on hardware
// that cannot compute per-pixel waveforms.
static const uint8_t kBayer4[4][4] = {
    {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5},
};

// level 0 = solid black .. 16 = solid white
static inline void px_dither(int x, int y, uint8_t level)
{
  px(x, y, level <= kBayer4[y & 3][x & 3]);
}

static bool g_dither_anim = false;

#define DCIRCS 6
static int dx_[DCIRCS], dy_[DCIRCS], dsx[DCIRCS], dsy[DCIRCS];
// White through black. Drawn in this order so the DARKEST is painted last and
// occludes the lighter ones — the earlier grey demo drew light last, which made
// overlaps look like the colours had swapped.
static const uint8_t kDitherLevel[DCIRCS] = {16, 13, 10, 6, 3, 0};

static void dither_anim_init(void)
{
  for (int i = 0; i < DCIRCS; i++) {
    dx_[i] = 70 + (i % 3) * 140;
    dy_[i] = 120 + i * 105;
    dsx[i] = (i & 1) ? 1 : -1;
    dsy[i] = (i & 2) ? 1 : -1;
  }
}

static void dither_anim_render(void)
{
  const int r = 52;
  for (int i = 0; i < DCIRCS; i++) {
    dx_[i] += dsx[i] * 22;
    dy_[i] += dsy[i] * 30;
    if (dx_[i] < r) { dx_[i] = r; dsx[i] = 1; }
    if (dx_[i] > LCD_W - r) { dx_[i] = LCD_W - r; dsx[i] = -1; }
    if (dy_[i] < r) { dy_[i] = r; dsy[i] = 1; }
    if (dy_[i] > LCD_H - r) { dy_[i] = LCD_H - r; dsy[i] = -1; }
  }

  fb_fill(WHITE);
  draw_number(12, 12, g_wp.drive_tp, 4);  // which drive mode is running
  for (int i = 0; i < DCIRCS; i++) {
    uint8_t lv = kDitherLevel[i];
    for (int dy = -r; dy <= r; dy++) {
      int span = (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f);
      for (int dxp = -span; dxp <= span; dxp++)
        px_dither(dx_[i] + dxp, dy_[i] + dy, lv);
    }
    // The solid-white disc would be invisible on a white field, so outline it.
    if (lv >= 16)
      for (int a = 0; a < 360; a += 2) {
        float rad = (float)a * 0.0174533f;
        for (int t = 0; t < 3; t++)
          px(dx_[i] + (int)((r - t) * cosf(rad)), dy_[i] + (int)((r - t) * sinf(rad)),
             true);
      }
  }
}

// ---- animation --------------------------------------------------------------
static int bx = 200, by = 200, bsx = 1, bsy = 1;
static const int kRadius = 48;
// Sized against the 480x800 canvas, not copied from the 5.79. There the disc
// stepped 26 px across 272 — a tenth of the panel per frame. 24 px across 800
// looked like a crawl even at 6 fps, because the frame rate was fine and the
// step was not.
static const int kStep = 60;

static void advance(void)
{
  bx += bsx * (kStep / 2);
  by += bsy * kStep;
  if (bx < kRadius) { bx = kRadius; bsx = 1; }
  if (bx > LCD_W - kRadius) { bx = LCD_W - kRadius; bsx = -1; }
  if (by < kRadius) { by = kRadius; bsy = 1; }
  if (by > LCD_H - kRadius) { by = LCD_H - kRadius; bsy = -1; }
}


static void render(void)
{
  fb_fill(WHITE);
  // Top-left: drive frames, then last measured total. Big enough to read from
  // across the bench.
  draw_number(24, 24, g_wp.drive_tp, 6);
  draw_number(24, 90, predicted_busy_ms(), 6);
  draw_number(24, 156, g_last_tot_us / 1000UL, 6);
  fill_disc(bx, by, kRadius, true);
}

// ---- LongTris ---------------------------------------------------------------
// Ported from ../crowpanel-tetris, which ran on a 272x792 panel. Geometry is
// re-derived rather than scaled: columns span the full 480 width at 48 px each,
// and the rows follow from what fits the remaining height.
//
// The multi-piece stream is the part that makes this LongTris and not Tetris:
// several pieces fall at once and the player controls the OLDEST, so the board
// fills from above while you are still placing what is below.
//
// Not ported: special pieces, difficulty tiers, the pause menu and high-score
// persistence. Those need the font blob and a menu system the bench has no use
// for.
#define T_COLS 10
#define T_CW (LCD_W / T_COLS)              // 48 - columns reach both edges
#define T_CH 40
#define T_ROWS ((LCD_H - 40) / T_CH)       // 19
#define T_OY (LCD_H - T_ROWS * T_CH)       // HUD band above the board
#define T_MAXLIVE 8
#define T_GAP 6                            // rows a piece falls before the next

static uint8_t t_grid[T_ROWS][T_COLS];
typedef struct { int8_t type, rot; int8_t x, y; bool on; } TLive;
static TLive t_live[T_MAXLIVE];
static int t_nlive;
static uint32_t t_score, t_lines;
static bool t_over;

// 7 tetrominoes, 4 rotations, 4 cells each as (x,y) offsets in a 4x4 box.
static const int8_t T_SHAPES[7][4][4][2] = {
    {{{0,1},{1,1},{2,1},{3,1}}, {{2,0},{2,1},{2,2},{2,3}},
     {{0,2},{1,2},{2,2},{3,2}}, {{1,0},{1,1},{1,2},{1,3}}},   // I
    {{{1,0},{2,0},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{2,1}},
     {{1,0},{2,0},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{2,1}}},   // O
    {{{1,0},{0,1},{1,1},{2,1}}, {{1,0},{1,1},{2,1},{1,2}},
     {{0,1},{1,1},{2,1},{1,2}}, {{1,0},{0,1},{1,1},{1,2}}},   // T
    {{{1,0},{2,0},{0,1},{1,1}}, {{1,0},{1,1},{2,1},{2,2}},
     {{1,1},{2,1},{0,2},{1,2}}, {{0,0},{0,1},{1,1},{1,2}}},   // S
    {{{0,0},{1,0},{1,1},{2,1}}, {{2,0},{1,1},{2,1},{1,2}},
     {{0,1},{1,1},{1,2},{2,2}}, {{1,0},{0,1},{1,1},{0,2}}},   // Z
    {{{0,0},{0,1},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{1,2}},
     {{0,1},{1,1},{2,1},{2,2}}, {{1,0},{1,1},{0,2},{1,2}}},   // J
    {{{2,0},{0,1},{1,1},{2,1}}, {{1,0},{1,1},{1,2},{2,2}},
     {{0,1},{1,1},{2,1},{0,2}}, {{0,0},{1,0},{1,1},{1,2}}},   // L
};

// Collides with the walls, the floor, or settled blocks. `self` is excluded from
// the live-piece test so a piece never blocks itself.
static bool t_hits(int self, int type, int rot, int x, int y)
{
  for (int i = 0; i < 4; i++) {
    int cx = x + T_SHAPES[type][rot][i][0];
    int cy = y + T_SHAPES[type][rot][i][1];
    if (cx < 0 || cx >= T_COLS || cy >= T_ROWS) return true;
    if (cy >= 0 && t_grid[cy][cx]) return true;
    for (int j = 0; j < T_MAXLIVE; j++) {
      if (j == self || !t_live[j].on) continue;
      for (int k = 0; k < 4; k++)
        if (cx == t_live[j].x + T_SHAPES[t_live[j].type][t_live[j].rot][k][0] &&
            cy == t_live[j].y + T_SHAPES[t_live[j].type][t_live[j].rot][k][1])
          return true;
    }
  }
  return false;
}

static void t_lock(int i)
{
  for (int k = 0; k < 4; k++) {
    int cx = t_live[i].x + T_SHAPES[t_live[i].type][t_live[i].rot][k][0];
    int cy = t_live[i].y + T_SHAPES[t_live[i].type][t_live[i].rot][k][1];
    if (cy < 0) t_over = true;               // locked above the ceiling
    else if (cx >= 0 && cx < T_COLS && cy < T_ROWS) t_grid[cy][cx] = 1;
  }
  t_live[i].on = false;
}

static int t_clear_lines(void)
{
  int n = 0;
  for (int r = T_ROWS - 1; r >= 0; r--) {
    bool full = true;
    for (int c = 0; c < T_COLS; c++)
      if (!t_grid[r][c]) { full = false; break; }
    if (!full) continue;
    for (int rr = r; rr > 0; rr--)
      memcpy(t_grid[rr], t_grid[rr - 1], T_COLS);
    memset(t_grid[0], 0, T_COLS);
    n++;
    r++;                                     // re-test the row that moved down
  }
  return n;
}

// Index of the piece the player drives: the oldest one still falling.
static int t_ctrl(void)
{
  for (int i = 0; i < T_MAXLIVE; i++)
    if (t_live[i].on) return i;
  return -1;
}

static void t_spawn(void)
{
  for (int i = 0; i < T_MAXLIVE; i++) {
    if (t_live[i].on) continue;
    t_live[i].type = (int8_t)(esp_random() % 7);
    t_live[i].rot = 0;
    t_live[i].x = (int8_t)((T_COLS - 4) / 2);
    t_live[i].y = -1;
    t_live[i].on = true;
    t_nlive++;
    return;
  }
}

// Youngest piece's depth, so the next one enters only after a gap opens.
static int t_newest_y(void)
{
  int y = -99;
  for (int i = 0; i < T_MAXLIVE; i++)
    if (t_live[i].on && t_live[i].y > y) y = t_live[i].y;
  return y;
}

// Stable ~70% "broken pencil" mask: fine noise plus 3x2 clumps, so lines come
// out dashed and erased-looking rather than speckled. Position-based and
// therefore identical every frame — which matters twice over here, because a
// mask that changed frame to frame would also defeat the differential update.
static bool t_ink(int x, int y)
{
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  uint32_t clump = (uint32_t)(x / 3) * 1103515245u + (uint32_t)(y / 2) * 12345u;
  return ((h ^ clump ^ (clump >> 7)) % 100u) < 70u;
}

static void t_hline(int x, int y, int w)
{
  for (int xx = x; xx < x + w; xx++) px(xx, y, t_ink(xx, y));
}

static void t_vline(int x, int y, int h)
{
  for (int yy = y; yy < y + h; yy++) px(x, yy, t_ink(x, yy));
}

// EVERY cell is drawn, not just the filled ones: the pencil grid over the whole
// board is the look. Filled blocks add the same pencil mask inset 3 px, so they
// read as shaded-in squares rather than solid ink.
static void t_cell(int c, int r, bool filled)
{
  int x = c * T_CW, y = T_OY + r * T_CH;
  fill_rect(x, y, T_CW, T_CH, false);          // white paper

  t_hline(x, y, T_CW);
  t_hline(x, y + T_CH - 1, T_CW);
  t_vline(x, y, T_CH);
  t_vline(x + T_CW - 1, y, T_CH);

  if (!filled) return;
  const int inset = 3;
  for (int yy = y + inset; yy < y + T_CH - inset; yy++)
    for (int xx = x + inset; xx < x + T_CW - inset; xx++)
      px(xx, yy, t_ink(xx, yy));
}

static void t_render(void)
{
  fb_fill(WHITE);
  // On game over the HUD inverts to a solid black band. Without it the board
  // simply stops moving and ignores input, which is indistinguishable from a
  // crash — and was read as one.
  if (t_over) fill_rect(0, 0, LCD_W, T_OY - 2, true);
  draw_number(8, 6, t_score, 4);
  draw_number(LCD_W - 120, 6, t_lines, 4);
  // Board edge, so the playfield reads as a container rather than floating.
  fill_rect(0, T_OY - 2, LCD_W, 2, true);

  // Draw every cell so the pencil grid covers the whole board, then shade the
  // live pieces over the top.
  for (int r = 0; r < T_ROWS; r++)
    for (int c = 0; c < T_COLS; c++) t_cell(c, r, t_grid[r][c] != 0);

  for (int i = 0; i < T_MAXLIVE; i++) {
    if (!t_live[i].on) continue;
    for (int k = 0; k < 4; k++) {
      int cx = t_live[i].x + T_SHAPES[t_live[i].type][t_live[i].rot][k][0];
      int cy = t_live[i].y + T_SHAPES[t_live[i].type][t_live[i].rot][k][1];
      if (cy >= 0 && cy < T_ROWS && cx >= 0 && cx < T_COLS) t_cell(cx, cy, true);
    }
  }
}

static void t_new_game(void)
{
  memset(t_grid, 0, sizeof(t_grid));
  for (int i = 0; i < T_MAXLIVE; i++) t_live[i].on = false;
  t_nlive = 0;
  t_score = 0;
  t_lines = 0;
  t_over = false;
  t_spawn();
}

// Defined further down with the panel plumbing they belong to; the game needs
// them here because setup() enters the game before those definitions.
static void panel_seed(void);
static uint8_t eff_seq(void);

// The game loop, factored out so both the `tetris` command and setup()
// can enter it. Returns when the player quits, dropping into the bench.
static void tetris_run(int dr)
{
  if (dr < 1) dr = 1;
  g_running = false;
  g_gray_anim = false;
  g_dither_anim = false;
  g_draw_gray = false;
  uint8_t saved = g_wp.drive_tp;
  g_wp.drive_tp = (uint8_t)dr;
  g_custom = true;
  arm_wave();
  EPD_AnalogPower(true);

  // panel_seed() writes OLD inverted, so every pixel actually transitions to
  // white and the previous image is gone. Writing old == new instead drives
  // nothing at all: the glass keeps whatever was on it, and the differential
  // frames that follow are then diffed against a g_prev the panel does not
  // match — which shows up as leftover junk and an inverted-looking board.
  fb_fill(WHITE);
  panel_seed();

  t_new_game();
  // Drop anything already buffered before reading input. keys.py sends 'q' when
  // it is interrupted, and that byte survives in the CDC buffer across a reset —
  // so the game would start, immediately read the stale quit, and hand straight
  // back to the bench. Which looked exactly like "it booted into the demo".
  while (Serial.available()) (void)Serial.read();
  Serial.println("TETRIS  a/d move  w rotate  s drop  space hard  r restart  q quit");

  uint32_t drop_ms = 700, last_drop = millis();
  bool quit = false, announced = false;
  while (!quit) {
    if (t_over && !announced) {
      announced = true;
      Serial.printf("GAME OVER — score %lu, lines %lu. `r` restarts, `q` quits.\n",
                    (unsigned long)t_score, (unsigned long)t_lines);
    }
    if (!t_over) announced = false;
    while (Serial.available()) {
      int ch = Serial.read();
      int i = t_ctrl();
      if (ch == 'q' || ch == 'Q') { quit = true; break; }
      if (ch == 'r' || ch == 'R') { t_new_game(); drop_ms = 700; continue; }
      if (t_over || i < 0) continue;
      TLive *p = &t_live[i];
      if (ch == 'a' || ch == 'A') {
        if (!t_hits(i, p->type, p->rot, p->x - 1, p->y)) p->x--;
      } else if (ch == 'd' || ch == 'D') {
        if (!t_hits(i, p->type, p->rot, p->x + 1, p->y)) p->x++;
      } else if (ch == 'w' || ch == 'W') {
        int nr = (p->rot + 1) & 3;
        // Wall kick: try in place, then nudged one column each way.
        if (!t_hits(i, p->type, nr, p->x, p->y)) p->rot = (int8_t)nr;
        else if (!t_hits(i, p->type, nr, p->x - 1, p->y)) { p->rot = (int8_t)nr; p->x--; }
        else if (!t_hits(i, p->type, nr, p->x + 1, p->y)) { p->rot = (int8_t)nr; p->x++; }
      } else if (ch == 's' || ch == 'S') {
        if (!t_hits(i, p->type, p->rot, p->x, p->y + 1)) p->y++;
        last_drop = millis();
      } else if (ch == ' ') {
        while (!t_hits(i, p->type, p->rot, p->x, p->y + 1)) p->y++;
        t_lock(i);
        t_nlive--;
        uint32_t n = (uint32_t)t_clear_lines();
        if (n) { t_lines += n; t_score += n * n * 100; }
      }
    }
    if (quit) break;

    if (!t_over && millis() - last_drop >= drop_ms) {
      last_drop = millis();
      // Fall from the bottom up so a piece never lands on one that is about to
      // move out of the way.
      for (int i = T_MAXLIVE - 1; i >= 0; i--) {
        if (!t_live[i].on) continue;
        TLive *p = &t_live[i];
        if (!t_hits(i, p->type, p->rot, p->x, p->y + 1)) p->y++;
        else {
          t_lock(i);
          t_nlive--;
          uint32_t n = (uint32_t)t_clear_lines();
          if (n) { t_lines += n; t_score += n * n * 100; }
        }
      }
      if (t_nlive == 0 || t_newest_y() >= T_GAP) {
        if (t_nlive < T_MAXLIVE) t_spawn();
      }
      // Speed up with the pile, floored so it stays playable at ~12 fps.
      // Computed as a subtraction that cannot wrap: drop_ms is unsigned, and
      // 700 - lines*25 goes negative from 29 lines on, which would underflow to
      // a huge interval and stop the pieces falling entirely.
      uint32_t step = t_lines * 25;
      drop_ms = (step >= 580) ? 120 : (700 - step);
    }

    t_render();
    EPD_SetRamOption(RAM_NORMAL, RAM_NORMAL);
    EPD_WriteNew(g_fb);
    EPD_WriteOld(g_prev, false);
    uint32_t b = 0;
    EPD_Update(eff_seq(), &b);
    memcpy(g_prev, g_fb, EPD_PLANE_BYTES);
    // This loop never returns to loop(), so yield explicitly or core 1's IDLE
    // task never runs and the watchdog panics the board — the same trap that
    // bit the stream receive task.
    delay(1);
  }

  g_wp.drive_tp = saved;
  arm_wave();
  Serial.printf("TETRIS over — score %lu, lines %lu\n", (unsigned long)t_score,
                (unsigned long)t_lines);
}

// ---- waveform arming --------------------------------------------------------
static uint32_t g_fr_period_us = 0;  // refined by calib; 0 = use the law below

// Frame period as a function of the FR nibble. Measured on this panel by the
// first calib sweep: the periods land on 79266/N microseconds for N = 1..8
// (79.3, 39.6, 26.5, 19.8, 15.9, 13.2, 11.3, 9.9 ms). N = 9 broke the series at
// 52.9 ms, so treat anything above 8 as unknown until calib says otherwise.
//
// Having this as a closed form rather than "0 until you run calib" means the
// panel shows a real number the moment it boots.
static uint32_t fr_period_us(uint8_t fr)
{
  if (g_fr_period_us) return g_fr_period_us;
  uint8_t n = fr & 0x0F;
  if (n == 0) return 0;
  return 79266UL / n;
}

static uint32_t predicted_busy_ms(void)
{
  if (!g_custom) return g_last_busy_us / 1000UL;
  uint32_t per = fr_period_us(g_wp.fr);
  if (!per) return g_last_busy_us / 1000UL;
  return (uint32_t)((uint64_t)wave1677_frames(g_lut) * per / 1000ULL);
}

static void arm_wave(void)
{
  // Every regime change routes through here — `drive N`, `fr N`, any voltage
  // command, entering or leaving the game. Averaging frames from either side of
  // one reports a rate that was never on the glass.
  fps_reset();
  if (!g_custom) return;
  wave1677_build(g_lut, &g_wp);
  EPD_WriteLut(g_lut);
  EPD_WriteVoltages(g_wp.vgh, g_wp.vsh1, g_wp.vsh2, g_wp.vsl, g_wp.vcom);
}

static uint8_t eff_seq(void)
{
  if (g_seq) return g_seq;
  return g_custom ? 0x0C : 0xFC;  // fast rails-up path, or the vendor partial
}

// ---- panel ------------------------------------------------------------------
static void panel_clear_white(void)
{
  fb_fill(WHITE);
  EPD_WriteNew(g_fb);
  // No inverted OLD write here: EPD_UpdateFullFast sets 0x21 to bypass the OLD
  // plane as 0, so the controller treats every pixel as black-going-white and
  // drives the lot. Writing an inverted plane as well would be 48 KB of SPI the
  // hardware is going to ignore.
  uint32_t b = 0;
  EPD_UpdateFullFast(&b);
  EPD_WriteOld(g_fb, false);  // planes agree again
  memcpy(g_prev, g_fb, sizeof(g_prev));
  Serial.printf("CLEAR busy_ms=%lu\n", (unsigned long)(b / 1000UL));
}

// Seed: drive the current framebuffer onto the glass and leave both planes
// agreeing with it, so the next differential update starts coherent.
static void panel_seed(void)
{
  EPD_WriteNew(g_fb);
  EPD_WriteOld(g_fb, true);
  uint32_t b = 0;
  EPD_UpdatePartial(&b);
  EPD_WriteOld(g_fb, false);
  memcpy(g_prev, g_fb, sizeof(g_prev));
  arm_wave();  // the vendor sequences reload the OTP LUT, so re-arm after
}

static void show_frame(void)
{
  uint32_t t0 = micros();

  uint32_t s0 = micros();
  EPD_WriteNew(g_fb);
  EPD_WriteOld(g_prev, false);  // true differential
  g_last_spi_us = micros() - s0;

  // 0x21 is sticky, and a full clear leaves it bypassing the OLD plane as 0.
  // Without restoring Normal here every frame would treat the whole panel as
  // changed and drive it — a differential update in name only. Three bytes.
  EPD_SetRamOption(RAM_NORMAL, RAM_NORMAL);

  uint32_t busy = 0;
  EPD_Update(eff_seq(), &busy);
  g_last_busy_us = busy;
  g_last_tot_us = micros() - t0;

  memcpy(g_prev, g_fb, sizeof(g_prev));

  fps_tick();

  // Every 8th frame, not every frame. On native USB CDC a write blocks until the
  // host drains it, so logging per frame ties the animation to whether anything
  // is listening — with no terminal attached the panel dropped from ~6 fps to
  // ~1 fps, which looks exactly like a display problem and is not one.
  // setTxTimeoutMs(0) in setup() stops the blocking; this keeps the noise down.
  //
  // The 5.79 sibling has the same throttle at 32, because it logs over a 115200
  // UART where the line costs ~14.8 ms of wire time rather than a USB bulk write.
  if ((g_frame & 7) == 0)
    Serial.printf("RESULT f=%lu custom=%u seq=0x%02X drive=%u spi_ms=%lu busy_ms=%lu "
                  "tot_ms=%lu to=%u fps=%s\n",
                  (unsigned long)g_frame, (unsigned)g_custom, (unsigned)eff_seq(),
                  (unsigned)g_wp.drive_tp, (unsigned long)(g_last_spi_us / 1000UL),
                  (unsigned long)(g_last_busy_us / 1000UL),
                  (unsigned long)(g_last_tot_us / 1000UL), (unsigned)EPD_BusyTimedOut(),
                  g_fps_num[0] ? g_fps_num : "-");
}

// ---- probes -----------------------------------------------------------------
// All-VSS waveform: full time cost, zero field, image cannot change. Sweeping
// the phase length gives the frame period as a slope and the fixed per-update
// cost as an intercept. This is the measurement that should come before any
// optimisation — on the 5.79 it is what turned guesswork into arithmetic.
static void run_calib(void)
{
  // Nibbles 1..F. The first sweep stopped at 9 and found period = 79.3/N ms,
  // which left the question of whether A..F continue the series and go below
  // 9.9 ms/frame — worth knowing, since that is the waveform floor.
  static const uint8_t kFr[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                                0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  static const uint8_t kTp[] = {1, 2, 4, 8, 16, 32};

  Serial.println("CALIB seq=0x0C — all-VSS waveform, glass must not change");
  Serial.println("  fr    tp=1    tp=2    tp=4    tp=8   tp=16   tp=32   us/frame  fixed_ms");

  bool saved = g_custom;
  g_custom = true;
  EPD_AnalogPower(true);

  for (uint8_t i = 0; i < sizeof(kFr); i++) {
    uint32_t us[6];
    for (uint8_t j = 0; j < sizeof(kTp); j++) {
      wave1677_build_null(g_lut, kTp[j], kFr[i]);
      EPD_WriteLut(g_lut);
      uint32_t b = 0;
      EPD_Update(0x0C, &b);
      us[j] = b;
      delay(20);
    }
    int32_t slope = (int32_t)(us[5] - us[0]) / (int32_t)(kTp[5] - kTp[0]);
    int32_t fixed = (int32_t)us[0] - slope * (int32_t)kTp[0];
    Serial.printf("  0x%02X", (unsigned)kFr[i]);
    for (uint8_t j = 0; j < 6; j++) Serial.printf("%7lu", (unsigned long)(us[j] / 1000UL));
    Serial.printf("  %8ld  %8.1f\n", (long)slope, (double)fixed / 1000.0);
    if (kFr[i] == g_wp.fr && slope > 0) g_fr_period_us = (uint32_t)slope;
  }

  g_custom = saved;
  arm_wave();
  Serial.println("CALIB done — us/frame is the frame period; fixed_ms is what no");
  Serial.println("waveform change can remove, only a different activate byte can.");
}

// Same null waveform, only the 0x22 byte varies: isolates power sequencing.
static void run_seqscan(void)
{
  static const uint8_t kSeq[] = {0xFC, 0xCF, 0xCC, 0x8C, 0x0C};
  static const char *kWhat[] = {
      // b4 is SET in 0xFC, so it reloads the factory LUT and its timing reflects
      // the OTP waveform, NOT the null probe. Kept as the baseline to beat.
      "vendor partial - RELOADS OTP LUT, so this row is the factory waveform",
      "ramp up, display Mode 2, ramp down",
      "ramp up, display, rails left up",
      "clock + display (needs rails up)",
      "display only (needs clock + rails up)",
  };

  Serial.println("SEQSCAN — null waveform, glass must not change");
  Serial.println("  seq   tp=1   tp=32  us/frame  fixed_ms  meaning");
  bool saved = g_custom;
  g_custom = true;

  for (uint8_t i = 0; i < sizeof(kSeq); i++) {
    uint32_t a = 0, b = 0;
    wave1677_build_null(g_lut, 1, g_wp.fr);
    EPD_WriteLut(g_lut);
    EPD_Update(kSeq[i], &a);
    delay(20);
    wave1677_build_null(g_lut, 32, g_wp.fr);
    EPD_WriteLut(g_lut);
    EPD_Update(kSeq[i], &b);
    delay(20);
    int32_t slope = (int32_t)(b - a) / 31;
    int32_t fixed = (int32_t)a - slope;
    Serial.printf("  0x%02X %6lu %6lu  %8ld  %8.1f  %s\n", (unsigned)kSeq[i],
                  (unsigned long)(a / 1000UL), (unsigned long)(b / 1000UL), (long)slope,
                  (double)fixed / 1000.0, kWhat[i]);
  }

  g_custom = saved;
  arm_wave();
  Serial.println("SEQSCAN done — the cheapest row is the per-frame floor.");
}

// ---- serial -----------------------------------------------------------------
static void print_help(void)
{
  Serial.println("commands:");
  Serial.println("  help | status | wave");
  Serial.println("  clear | white | black    full-panel paints");
  Serial.println("  run | stop | step");
  Serial.println("  calib | seqscan          non-destructive measurements");
  Serial.println("  custom 0|1               vendor partial vs our LUT");
  Serial.println("  seq X | seq auto         force the 0x22 activate byte");
  Serial.println("  drive N | settle N | rp N | fr X | pol 0|1");
  Serial.println("  spi hw N | spi bitbang");
}

static void print_status(void)
{
  Serial.printf("STATUS %ux%u custom=%u seq=0x%02X drive=%u settle=%u rp=%u fr=0x%02X "
                "pol=%u spi=%s frames=%u fr_period_us=%lu run=%u frame=%lu\n",
                EPD_W, EPD_H, (unsigned)g_custom, (unsigned)eff_seq(),
                (unsigned)g_wp.drive_tp, (unsigned)g_wp.settle_tp, (unsigned)g_wp.rp,
                (unsigned)g_wp.fr, (unsigned)g_wp.invert, EPD_SpiPathName(),
                (unsigned)wave1677_frames(g_lut), (unsigned long)g_fr_period_us,
                (unsigned)g_running, (unsigned long)g_frame);
}

static bool parse_val(const char *s, long *out)
{
  while (*s == ' ') s++;
  if (!*s) return false;
  char *end = nullptr;
  long v;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) v = strtol(s + 2, &end, 16);
  else v = strtol(s, &end, 10);
  if (end == s) return false;
  *out = v;
  return true;
}

static void handle_cmd(char *s)
{
  while (*s == ' ') s++;
  if (!*s) return;

  if (!strcmp(s, "help")) { print_help(); return; }
  if (!strcmp(s, "status")) { print_status(); return; }
  if (!strcmp(s, "wave")) { wave1677_dump(g_lut); return; }
  if (!strcmp(s, "run")) { g_running = true; Serial.println("OK run"); return; }
  if (!strcmp(s, "stop")) {
    g_running = false;
    if (EPD_AnalogIsOn()) EPD_AnalogPower(false);
    Serial.println("OK stop (rails down)");
    return;
  }
  if (!strcmp(s, "step")) { g_step_once = true; return; }
  if (!strcmp(s, "calib")) { run_calib(); return; }
  // `stream` — play frames pushed from the host, in NATIVE landscape.
  //
  // 488 frames of 800x480 packed 1-bit is 23 MB, against 8 MB of flash, so the
  // clip cannot live on the device. The host decodes, crops, dithers and packs;
  // the device just receives 48000 bytes and displays them. Frames arrive in the
  // panel's own layout so there is no rotation and no conversion here.
  //
  // Flow control is one byte: the device sends 0x06 when it wants more data.
  // That makes playback self-pacing — if USB cannot keep up, it plays slower
  // rather than tearing or desynchronising.
  //
  // Receiving runs on its OWN TASK, on the other core, filling one of two buffers
  // while the panel displays the other. Measured with the two in series: USB
  // alone 4.9 fps, panel alone 8.6 fps, together 3.1 — exactly 1/(1/4.9 + 1/8.6).
  // Overlapped, the slower of the two sets the rate instead of their sum.
  //
  // `sink` is the same protocol with the panel writes removed: it measures the
  // host link on its own, so a slow frame rate can be attributed to USB or to the
  // display instead of guessed at.
  if (!strcmp(s, "stream") || !strcmp(s, "sink")) {
    const bool display = (s[1] == 't');
    g_running = false;
    g_gray_anim = false;
    g_dither_anim = false;
    g_draw_gray = false;
    g_custom = true;
    arm_wave();
    EPD_AnalogPower(true);

    // g_fb_lsb is only used by 4-level grey mode, which cannot be running here,
    // so it serves as the second frame buffer rather than costing another 48 KB.
    g_sbuf[0] = g_fb;
    g_sbuf[1] = g_fb_lsb;
    // Deltas are relative, so both ends must agree on the starting image. White
    // is that agreed state, and the host clears the panel to white before it
    // begins — otherwise frame 1 is reconstructed against the wrong base and the
    // error persists for the whole clip.
    memset(g_fb, 0xFF, EPD_PLANE_BYTES);
    memset(g_fb_lsb, 0xFF, EPD_PLANE_BYTES);
    memset(g_prev, 0xFF, EPD_PLANE_BYTES);
    g_srx_fail = false;
    g_srx_run = true;
    g_sem_free = xSemaphoreCreateCounting(2, 2);
    g_sem_ready = xSemaphoreCreateCounting(2, 0);

    // The request token must be a byte that cannot appear in ordinary log text.
    // It was 'R', and the banner "STREAM ready" contains an R — the host saw
    // that, started sending immediately, and the stream desynchronised before it
    // began. 0x06 (ACK) is never printed.
    Serial.println("STREAM ready");
    Serial.flush();
    delay(50);

    // Core 0: the Arduino loop runs on core 1, so the receiver gets a core to
    // itself and never competes with the SPI transfer for CPU.
    TaskHandle_t rx = NULL;
    xTaskCreatePinnedToCore(stream_rx_task, "srx", 4096, NULL, 2, &rx, 0);

    uint32_t frames = 0, t0 = millis();
    for (uint32_t c = 0;; c++) {
      if (xSemaphoreTake(g_sem_ready, pdMS_TO_TICKS(6000)) != pdTRUE) break;
      if (g_srx_fail) break;
      uint8_t *buf = g_sbuf[c & 1];

      if (display) {
        EPD_SetRamOption(RAM_NORMAL, RAM_NORMAL);
        EPD_WriteNew(buf);
        EPD_WriteOld(g_prev, false);
        uint32_t busy = 0;
        EPD_Update(eff_seq(), &busy);
        memcpy(g_prev, buf, EPD_PLANE_BYTES);
      }
      frames++;
      xSemaphoreGive(g_sem_free);  // hand the buffer back to the receiver
    }

    // Unblock the receiver if it is parked waiting for a free buffer, then let it
    // exit before the semaphores it uses are destroyed.
    g_srx_run = false;
    xSemaphoreGive(g_sem_free);
    (void)rx;
    delay(200);  // let the receive task run to completion and delete itself
    vSemaphoreDelete(g_sem_free);
    vSemaphoreDelete(g_sem_ready);
    g_sem_free = g_sem_ready = NULL;

    uint32_t ms = millis() - t0;
    Serial.printf("STREAM done %lu frames in %lu ms (%lu.%lu fps)\n",
                  (unsigned long)frames, (unsigned long)ms,
                  (unsigned long)(frames * 1000UL / (ms ? ms : 1)),
                  (unsigned long)((frames * 10000UL / (ms ? ms : 1)) % 10));
    Serial.setTimeout(1000);
    return;
  }

  // `glevels [step]` — four discs, four codes, graduated drive, NO reset.
  // Proof of whether flash-free multi-level grey is possible on this controller.
  if (!strncmp(s, "glevels", 7) && (s[7] == ' ' || !s[7])) {
    int stp = 6;
    if (s[7]) stp = atoi(s + 8);
    if (stp < 1) stp = 1;
    g_running = false;
    g_gray_anim = false;
    g_dither_anim = false;

    // Start from a known white field: without the reset the levels are relative,
    // so the starting state IS the reference.
    g_draw_gray = true;
    memset(g_fb, 0xFF, sizeof(g_fb));
    memset(g_fb_lsb, 0xFF, sizeof(g_fb_lsb));
    EPD_WriteNew(g_fb);
    EPD_WriteOld(g_fb, true);
    EPD_SetRamOption(RAM_NORMAL, RAM_NORMAL);
    uint32_t bw = 0;
    EPD_Update(0xD7, &bw);

    // Now the four codes, on that white field.
    const int r = 52;
    for (uint8_t c = 0; c < 4; c++) {
      gray_disc(150, 110 + c * 160, r, c);
      draw_number(250, 86 + c * 160, c, 6);
    }

    wave1677_build_levels(g_lut, (uint8_t)stp, g_wp.fr, g_wp.invert);
    EPD_WriteLut(g_lut);
    EPD_WriteVoltages(g_wp.vgh, g_wp.vsh1, g_wp.vsh2, g_wp.vsl, g_wp.vcom);
    EPD_SetRamOption(RAM_NORMAL, RAM_NORMAL);
    EPD_WriteNew(g_fb);
    EPD_WriteOldMasked(g_fb_lsb, nullptr);
    uint32_t b = 0;
    EPD_Update(0x04, &b);  // Mode 1 display, rails up, no ramp
    Serial.printf("GLEVELS step=%d frames=%u busy_ms=%lu (white prep %lu ms) - "
                  "codes 0..3, no reset, should NOT flash\n",
                  stp, (unsigned)wave1677_frames(g_lut), (unsigned long)(b / 1000UL),
                  (unsigned long)(bw / 1000UL));
    g_draw_gray = false;
    arm_wave();
    return;
  }
  // `grun [reset] [step]` — bouncing discs in 4-level grey.
  // `dcirc` — six discs, white through black, dithered on the mono path.
  if (!strcmp(s, "dcirc")) {
    g_gray_anim = false;
    g_draw_gray = false;
    g_custom = true;
    dither_anim_init();
    g_dither_anim = true;
    arm_wave();
    g_running = true;
    Serial.printf("OK dcirc - 6 discs, drive=%u, no flash\n", (unsigned)g_wp.drive_tp);
    return;
  }
  if (!strncmp(s, "grun", 4) && (s[4] == ' ' || !s[4])) {
    int rst = 12, stp = 4;
    if (s[4]) sscanf(s + 5, "%d %d", &rst, &stp);
    if (rst < 1) rst = 1;
    if (stp < 1) stp = 1;
    g_ganim_reset = (uint8_t)rst;
    g_ganim_step = (uint8_t)stp;
    // Arm the grey ramp once; the animation reuses it every frame.
    wave1677_build_gray(g_lut, g_ganim_reset, g_ganim_step, g_wp.fr, g_wp.invert);
    EPD_WriteLut(g_lut);
    EPD_WriteVoltages(g_wp.vgh, g_wp.vsh1, g_wp.vsh2, g_wp.vsl, g_wp.vcom);
    EPD_AnalogPower(true);  // 0x04 assumes the rails are already up
    gray_anim_init();
    g_draw_gray = true;
    g_gray_anim = true;
    g_running = true;
    Serial.printf("OK grun reset=%d step=%d frames=%u predicted_ms=%lu\n", rst, stp,
                  (unsigned)wave1677_frames(g_lut),
                  (unsigned long)((uint64_t)wave1677_frames(g_lut) *
                                  fr_period_us(g_wp.fr) / 1000ULL));
    return;
  }
  if (!strcmp(s, "gstop")) {
    g_dither_anim = false;
    g_gray_anim = false;
    g_draw_gray = false;
    g_running = false;
    arm_wave();
    Serial.println("OK gstop — `deghost 2` to clean up before mono");
    return;
  }
  // `shades` — the measured palette in order, as a gradient. This is the one to
  // look at to judge whether the 10 levels really are evenly spaced.
  if (!strncmp(s, "shades", 6)) {
    int rst = 24, stp = 6;
    if (s[6] == ' ') sscanf(s + 7, "%d %d", &rst, &stp);
    g_running = false;
    g_draw_gray = true;
    memset(g_fb, 0xFF, sizeof(g_fb));
    memset(g_fb_lsb, 0xFF, sizeof(g_fb_lsb));
    int bh = LCD_H / GRAY_LEVELS;
    for (int i = 0; i < GRAY_LEVELS; i++) {
      for (int y = i * bh; y < (i + 1) * bh; y++)
        for (int x = 60; x < LCD_W; x++) px_shade(x, y, (uint8_t)i);
      draw_number(8, i * bh + bh / 2 - 12, (uint32_t)i, 3);
    }
    wave1677_build_gray(g_lut, (uint8_t)rst, (uint8_t)stp, g_wp.fr, g_wp.invert);
    EPD_WriteLut(g_lut);
    EPD_WriteVoltages(g_wp.vgh, g_wp.vsh1, g_wp.vsh2, g_wp.vsl, g_wp.vcom);
    EPD_SetRamOption(RAM_NORMAL, RAM_NORMAL);
    EPD_WriteNew(g_fb);
    EPD_WriteOldMasked(g_fb_lsb, nullptr);
    uint32_t b = 0;
    EPD_Update(0xC7, &b);
    Serial.printf("SHADES %d levels dark->light, busy_ms=%lu\n", GRAY_LEVELS,
                  (unsigned long)(b / 1000UL));
    g_draw_gray = false;
    arm_wave();
    return;
  }
  // `ramp` or `ramp RESET STEP` — every native level and dither pair at once.
  if (!strncmp(s, "ramp", 4) && (s[4] == ' ' || !s[4])) {
    int rst = 24, stp = 6;
    if (s[4]) sscanf(s + 5, "%d %d", &rst, &stp);
    if (rst < 1) rst = 1;
    if (stp < 1) stp = 1;
    gray_ramp((uint8_t)rst, (uint8_t)stp);
    return;
  }
  // `gstill [RESET] [STEP]` — a host image in NATIVE 4-level grey.
  //
  // Two bitplanes arrive over the same chunked handshake the video stream uses:
  // 0x24 (LOW bit) first, then 0x26 (HIGH bit). Kept separate from `stream`
  // rather than made a mode of it, because grey is a full repaint with a common
  // reset — it flashes once per update, which is fine for a still and unwatchable
  // for video. Defaults match the `gray` demo that the levels were judged on.
  if (!strncmp(s, "gstill", 6) && (s[6] == ' ' || !s[6])) {
    int rst = 20, stp = 10;
    if (s[6]) sscanf(s + 7, "%d %d", &rst, &stp);
    if (rst < 1) rst = 1;
    if (stp < 1) stp = 1;
    g_running = false;
    g_gray_anim = false;
    g_dither_anim = false;
    g_draw_gray = false;

    Serial.println("GSTILL ready");
    Serial.flush();
    delay(50);
    if (!stream_read_exact(g_fb, EPD_PLANE_BYTES)) return;
    if (!stream_read_exact(g_fb_lsb, EPD_PLANE_BYTES)) return;

    wave1677_build_gray(g_lut, (uint8_t)rst, (uint8_t)stp, g_wp.fr, g_wp.invert);
    EPD_WriteLut(g_lut);
    EPD_WriteVoltages(g_wp.vgh, g_wp.vsh1, g_wp.vsh2, g_wp.vsl, g_wp.vcom);
    EPD_SetRamOption(RAM_NORMAL, RAM_NORMAL);
    EPD_WriteNew(g_fb);                     // 0x24 = LOW bit
    EPD_WriteOldMasked(g_fb_lsb, nullptr);  // 0x26 = HIGH bit
    uint32_t b = 0;
    EPD_Update(0xC7, &b);
    Serial.printf("GSTILL reset=%d step=%d frames=%u busy_ms=%lu\n", rst, stp,
                  (unsigned)wave1677_frames(g_lut), (unsigned long)(b / 1000UL));
    arm_wave();  // put the b/w waveform back
    return;
  }

  // `gray` or `gray RESET STEP` — sweep the ramp shape without reflashing.
  if (!strncmp(s, "gray", 4) && (s[4] == ' ' || !s[4])) {
    int rst = 20, stp = 10;
    if (s[4]) sscanf(s + 5, "%d %d", &rst, &stp);
    if (rst < 1) rst = 1;
    if (stp < 1) stp = 1;
    gray_demo((uint8_t)rst, (uint8_t)stp);
    return;
  }
  if (!strncmp(s, "cycle", 5)) {
    int secs = (s[5] == ' ') ? atoi(s + 6) : 8;
    if (secs <= 0) { g_cycle_ms = 0; Serial.println("OK cycle off"); return; }
    g_cycle_ms = (uint32_t)secs * 1000UL;
    g_cycle_i = 0;
    g_cycle_at = millis();
    g_wp.drive_tp = kCycle[0];
    arm_wave();
    g_running = true;
    Serial.printf("OK cycle: drive 12/5/2/1, %d s each\n", secs);
    return;
  }
  // `mux N` — scan only N gate lines instead of all 480. Run `calib` after it to
  // see what that does to the frame period, which is the whole question: if the
  // period scales, a short band is faster to DRIVE and not merely faster to
  // transfer, and a banner or a small sprite can run several times the full-panel
  // rate at the same contrast.
  if (!strncmp(s, "mux", 3) && (s[3] == ' ' || !s[3])) {
    int n = s[3] ? atoi(s + 4) : EPD_H;
    if (n < 8) n = 8;
    if (n > EPD_H) n = EPD_H;
    EPD_SetGateCount((uint16_t)n);
    Serial.printf("OK mux=%d gate lines\n", n);
    return;
  }
  // `rbench W H [drive] [frames]` — how fast can a REGION of this size run?
  //
  // Generated on-device and looped locally, so the host is entirely out of the
  // measurement: this is the panel's own ceiling for a banner or a sprite, with
  // no USB, no dithering and no delta encoding in the way. W is rounded to a
  // multiple of 8. Region is centred.
  if (!strncmp(s, "rbench", 6) && (s[6] == ' ' || !s[6])) {
    int w = 128, h = 128, dr = 2, nf = 60;
    if (s[6]) sscanf(s + 7, "%d %d %d %d", &w, &h, &dr, &nf);
    w = (w / 8) * 8;
    if (w < 8) w = 8;
    if (w > EPD_W) w = EPD_W;
    if (h < 1) h = 1;
    if (h > EPD_H) h = EPD_H;
    if (nf < 1) nf = 1;
    const uint16_t x0 = (uint16_t)(((EPD_W - w) / 2) & ~7);
    const uint16_t y0 = (uint16_t)((EPD_H - h) / 2);
    const uint32_t rb = (uint32_t)(w / 8) * h;  // bytes per plane for the region

    g_running = false;
    g_gray_anim = false;
    g_dither_anim = false;
    uint8_t saved = g_wp.drive_tp;
    g_wp.drive_tp = (uint8_t)(dr < 1 ? 1 : dr);
    g_custom = true;
    arm_wave();
    EPD_AnalogPower(true);

    // Seed the WHOLE panel to old == new == white first. The update always scans
    // every gate line, so any pixel left with old != new outside the window gets
    // re-driven on every single region update — which is what made the first
    // version glitch the entire display instead of touching only the region.
    // OLD must be written INVERTED or nothing transitions and the previous
    // image simply stays on the glass — old == new gives the waveform no work.
    fb_fill(WHITE);
    panel_seed();

    // Two small buffers carved out of the framebuffers we are not using.
    uint8_t *cur = g_fb, *prv = g_fb_lsb;
    memset(prv, 0xFF, rb);

    uint32_t spi_us = 0, busy_us = 0;
    uint32_t t0 = micros();
    int done = 0;
    for (int f = 0; f < nf; f++, done++) {
      // Any incoming byte stops it, so a long loop can be left running without
      // locking out the command interface.
      if (Serial.available()) break;
      // A bar sweeping across the region: every frame changes real pixels, so
      // this cannot flatter itself with an unchanged image.
      const int bar = (f * 8) % (w ? w : 1);
      for (int row = 0; row < h; row++) {
        uint8_t *r = cur + (uint32_t)row * (w / 8);
        memset(r, 0xFF, w / 8);
        for (int px = bar; px < bar + 16 && px < w; px++)
          r[px >> 3] &= (uint8_t)~(0x80 >> (px & 7));
      }
      uint32_t s0 = micros();
      EPD_SetRamOption(RAM_NORMAL, RAM_NORMAL);
      EPD_WriteRegion(0x24, cur, x0, y0, (uint16_t)w, (uint16_t)h);
      EPD_WriteRegion(0x26, prv, x0, y0, (uint16_t)w, (uint16_t)h);
      spi_us += micros() - s0;
      uint32_t b = 0;
      EPD_Update(eff_seq(), &b);
      busy_us += b;
      memcpy(prv, cur, rb);
    }
    uint32_t tot = micros() - t0;

    g_wp.drive_tp = saved;
    arm_wave();
    EPD_SetRamFull();
    Serial.printf("RBENCH %dx%d drive=%d %d frames  %lu us/frame  %lu.%02lu fps"
                  "  (spi %lu us, busy %lu us, bytes/plane %lu)\n",
                  w, h, dr, nf, (unsigned long)(tot / nf),
                  (unsigned long)(1000000UL * nf / tot),
                  (unsigned long)((100000000ULL * nf / tot) % 100),
                  (unsigned long)(spi_us / nf), (unsigned long)(busy_us / nf),
                  (unsigned long)rb);
    return;
  }
  // `rcirc [drive]` — the bouncing disc, but transferring only the DIRTY REGION.
  //
  // The disc and its previous position together cover a couple of thousand bytes
  // out of 96000, so SPI falls from ~20 ms to under 1 ms. At drive 5 that takes
  // the demo from ~12 fps to ~16, which is the entire benefit available: BUSY is
  // untouched at 59.5 ms and dominates completely. Radius is half the full-panel
  // demo's, which changes the transfer size and — as the numbers show — nothing
  // else that matters.
  //
  // Runs until any byte arrives on serial.
  if (!strncmp(s, "rcirc", 5) && (s[5] == ' ' || !s[5])) {
    int dr = s[5] ? atoi(s + 6) : 5;
    if (dr < 1) dr = 1;
    const int R = kRadius / 2;      // half the full-panel demo's disc
    const int STEPX = 15, STEPY = 30;
    static uint8_t rbuf[16384];

    g_running = false;
    g_gray_anim = false;
    g_dither_anim = false;
    g_draw_gray = false;
    uint8_t saved = g_wp.drive_tp;
    g_wp.drive_tp = (uint8_t)dr;
    g_custom = true;
    arm_wave();
    EPD_AnalogPower(true);

    // Seed old == new == white across the WHOLE panel. The update scans every
    // gate line regardless of the window, so anything left with old != new
    // outside it would be re-driven on every frame.
    // OLD must be written INVERTED or nothing transitions and the previous
    // image simply stays on the glass — old == new gives the waveform no work.
    fb_fill(WHITE);
    panel_seed();

    int cx = LCD_W / 2, cy = LCD_H / 2, sx = 1, sy = 1;
    fill_disc(cx, cy, R, true);

    uint32_t frames = 0, spi_us = 0, busy_us = 0, bytes = 0;
    uint32_t t0 = micros();
    while (!Serial.available()) {
      int ox = cx, oy = cy;
      fill_disc(ox, oy, R, false);   // erase where it was
      cx += sx * STEPX;
      cy += sy * STEPY;
      if (cx < R) { cx = R; sx = 1; }
      if (cx > LCD_W - R) { cx = LCD_W - R; sx = -1; }
      if (cy < R) { cy = R; sy = 1; }
      if (cy > LCD_H - R) { cy = LCD_H - R; sy = -1; }
      fill_disc(cx, cy, R, true);    // draw where it is

      // Dirty box covers both positions. px() maps logical (x,y) to native
      // (EPD_W-1-y, x), so logical Y becomes native X and vice versa.
      int lyl = (oy < cy ? oy : cy) - R, lyh = (oy > cy ? oy : cy) + R;
      int lxl = (ox < cx ? ox : cx) - R, lxh = (ox > cx ? ox : cx) + R;
      int nx0 = EPD_W - 1 - lyh, nx1 = EPD_W - 1 - lyl;
      int ny0 = lxl, ny1 = lxh;
      if (nx0 < 0) nx0 = 0;
      if (ny0 < 0) ny0 = 0;
      if (nx1 > EPD_W - 1) nx1 = EPD_W - 1;
      if (ny1 > EPD_H - 1) ny1 = EPD_H - 1;
      nx0 &= ~7;                     // windows must start and end on byte bounds
      nx1 |= 7;
      const uint16_t rw = (uint16_t)(nx1 - nx0 + 1);
      const uint16_t rh = (uint16_t)(ny1 - ny0 + 1);
      const uint32_t rbytes = (uint32_t)(rw / 8) * rh;

      uint32_t s0 = micros();
      EPD_SetRamOption(RAM_NORMAL, RAM_NORMAL);
      if (rbytes * 2 <= sizeof(rbuf)) {
        // Pack the sub-rectangle out of each full plane, tight rows.
        for (int p = 0; p < 2; p++) {
          const uint8_t *src = p ? g_prev : g_fb;
          uint8_t *dst = rbuf + (p ? rbytes : 0);
          for (uint16_t r = 0; r < rh; r++)
            memcpy(dst + (uint32_t)r * (rw / 8),
                   src + (uint32_t)(ny0 + r) * EPD_WBYTES + (nx0 >> 3), rw / 8);
        }
        EPD_WriteRegion(0x24, rbuf, (uint16_t)nx0, (uint16_t)ny0, rw, rh);
        EPD_WriteRegion(0x26, rbuf + rbytes, (uint16_t)nx0, (uint16_t)ny0, rw, rh);
        bytes += rbytes * 2;
      } else {
        EPD_WriteNew(g_fb);
        EPD_WriteOld(g_prev, false);
        bytes += EPD_PLANE_BYTES * 2;
      }
      spi_us += micros() - s0;

      uint32_t b = 0;
      EPD_Update(eff_seq(), &b);
      busy_us += b;
      memcpy(g_prev, g_fb, EPD_PLANE_BYTES);
      frames++;
    }
    uint32_t tot = micros() - t0;
    if (!frames) frames = 1;

    g_wp.drive_tp = saved;
    arm_wave();
    EPD_SetRamFull();
    Serial.printf("RCIRC r=%d drive=%d %lu frames  %lu us/frame  %lu.%02lu fps"
                  "  (spi %lu us, busy %lu us, %lu bytes/frame)\n",
                  R, dr, (unsigned long)frames, (unsigned long)(tot / frames),
                  (unsigned long)(1000000UL * frames / tot),
                  (unsigned long)((100000000ULL * frames / tot) % 100),
                  (unsigned long)(spi_us / frames),
                  (unsigned long)(busy_us / frames),
                  (unsigned long)(bytes / frames));
    return;
  }
  // `tetris [drive]` — LongTris, driven over serial with WASD.
  //   a / d  move      w  rotate      s  soft drop
  //   space  hard drop  r  restart     q  quit
  //
  // Input is read every loop but the board only redraws once per panel update,
  // so responsiveness is capped by the frame rate: ~12 fps at drive 5. Keys are
  // applied as they arrive rather than sampled, so a fast tap still registers.
  if (!strncmp(s, "tetris", 6) && (s[6] == ' ' || !s[6])) {
    tetris_run(s[6] ? atoi(s + 7) : 5);
    return;
  }
  if (!strcmp(s, "seqscan")) { run_seqscan(); return; }
  if (!strcmp(s, "clear")) { panel_clear_white(); return; }

  // Static black bar, drawn with a FULL Mode 1 refresh so it is at maximum
  // quality and does not move — for inspecting individual gate lines. Walk it
  // with `rect 460 4` and so on until it lands on the ones misbehaving; the
  // resulting X is the portrait column, which equals the panel's gate number.
  // `rect X W [M]` — M=1 (default) vendor full Mode 1 refresh; M=0 uses OUR
  // waveform and OUR voltages at speed.
  //
  // The distinction matters for voltage testing: 0xD7 has the load-LUT bit set,
  // so it pulls the factory waveform AND the factory voltage block out of OTP,
  // overwriting anything written to 0x03/0x04/0x2C beforehand. Any experiment
  // with the rails has to run on a custom LUT or it silently tests nothing.
  if (!strncmp(s, "rect", 4) && (s[4] == ' ' || !s[4])) {
    int x = LCD_W - 80, w = 80, mode = 1;
    if (s[4]) sscanf(s + 5, "%d %d %d", &x, &w, &mode);
    if (x < 0) x = 0;
    if (w < 1) w = 1;
    if (x + w > LCD_W) w = LCD_W - x;
    g_running = false;
    g_draw_gray = false;
    fb_fill(WHITE);
    fill_rect(x, 0, w, LCD_H, true);
    EPD_WriteNew(g_fb);
    EPD_WriteOld(g_fb, true);  // OLD = inverse, so every pixel is driven
    EPD_SetRamOption(RAM_NORMAL, RAM_NORMAL);
    uint32_t b = 0;
    if (mode) {
      EPD_Update(0xD7, &b);  // factory waveform and factory voltages
    } else {
      // Our LUT and our rails, armed AFTER any OTP load, then displayed with a
      // byte that does not reload the LUT.
      wave1677_build(g_lut, &g_wp);
      EPD_WriteLut(g_lut);
      EPD_WriteVoltages(g_wp.vgh, g_wp.vsh1, g_wp.vsh2, g_wp.vsl, g_wp.vcom);
      EPD_Update(0x0C, &b);
    }
    EPD_WriteOld(g_fb, false);
    memcpy(g_prev, g_fb, sizeof(g_prev));
    arm_wave();
    Serial.printf("RECT x=%d..%d (gates %d..%d) mode=%s vsh1=0x%02X vsl=0x%02X "
                  "busy_ms=%lu\n",
                  x, x + w - 1, x, x + w - 1, mode ? "vendor-full" : "custom-fast",
                  (unsigned)g_wp.vsh1, (unsigned)g_wp.vsl, (unsigned long)(b / 1000UL));
    return;
  }

  // `volts VSH1 VSL [VCOM] [VGH]`, all hex. Global — there is no per-line
  // control, so raising these drives every one of the 480 gate lines harder,
  // not just a weak pair. Ranges from the datasheet: VSH1 0x23..0x4B (9..17 V),
  // VSL 0x1A..0x3A (-9..-17 V), VGH 0x00/0x17 = 20 V which is already the
  // maximum and therefore not worth moving.
  if (!strncmp(s, "volts ", 6)) {
    unsigned a = 0, b2 = 0, c = 0, d = 0;
    int n = sscanf(s + 6, "%x %x %x %x", &a, &b2, &c, &d);
    // Refuse out-of-spec source rails rather than writing them. This used to
    // take any byte, which meant one mistyped hex digit drove all 480 gate
    // lines past the datasheet — for no gain, because above the maximum the
    // frame count is identical and the contrast difference is marginal.
    if (n >= 1 && (a < 0x23 || a > 0x4B)) {
      Serial.println("ERR vsh1 0x23..0x4B (9..17 V) - datasheet range");
      return;
    }
    if (n >= 2 && (b2 < 0x1A || b2 > 0x3A)) {
      Serial.println("ERR vsl 0x1A..0x3A (-9..-17 V) - datasheet range");
      return;
    }
    if (n >= 4 && d > 0x17) {
      Serial.println("ERR vgh <= 0x17 (20 V) - datasheet maximum");
      return;
    }
    if (n >= 1) g_wp.vsh1 = (uint8_t)a;
    if (n >= 2) g_wp.vsl = (uint8_t)b2;
    if (n >= 3) g_wp.vcom = (uint8_t)c;
    if (n >= 4) g_wp.vgh = (uint8_t)d;
    EPD_WriteVoltages(g_wp.vgh, g_wp.vsh1, g_wp.vsh2, g_wp.vsl, g_wp.vcom);
    arm_wave();
    Serial.printf("OK volts VGH=0x%02X VSH1=0x%02X VSH2=0x%02X VSL=0x%02X VCOM=0x%02X\n",
                  (unsigned)g_wp.vgh, (unsigned)g_wp.vsh1, (unsigned)g_wp.vsh2,
                  (unsigned)g_wp.vsl, (unsigned)g_wp.vcom);
    return;
  }

  // Temperature register. The init fakes 90 C (0x5A) to select the factory's
  // hottest and therefore SHORTEST waveform — which is where the speed comes
  // from, but also where the drive margin goes. Sweeping this down towards real
  // ambient (~0x19 = 25 C) trades speed back for margin, and turns "is the panel
  // faulty" into a threshold that can be measured.
  if (!strncmp(s, "temp ", 5)) {
    long v;
    if (!parse_val(s + 5, &v) || v < 0 || v > 255) {
      Serial.println("ERR temp 0..255 (0x5A = 90C fast, 0x19 = 25C ambient)");
      return;
    }
    EPD_WR_REG(0x1A);
    EPD_WR_DATA8((uint8_t)v);
    EPD_WR_REG(0x22);
    EPD_WR_DATA8(0x91);  // reload the factory LUT at the new temperature
    EPD_WR_REG(0x20);
    (void)EPD_WaitBusy_us(2000);
    arm_wave();
    Serial.printf("OK temp=0x%02X (%ld C nominal) — factory LUT reloaded\n",
                  (unsigned)v, v);
    return;
  }
  // Full black/white cycles on the factory Mode 1 waveform. A differential
  // update cannot lift residue from an earlier image — unchanged pixels are
  // never driven — so anything left over from previous text or grey levels
  // survives every partial refresh. Driving the whole panel hard to both
  // extremes is what actually clears it.
  if (!strncmp(s, "deghost", 7)) {
    int n = (s[7] == ' ') ? atoi(s + 8) : 3;
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    g_running = false;
    g_draw_gray = false;
    Serial.printf("DEGHOST %d cycles — this flashes, that is the point\n", n);
    for (int i = 0; i < n; i++) {
      for (int half = 0; half < 2; half++) {
        fb_fill(half ? WHITE : BLACK);
        EPD_WriteNew(g_fb);
        EPD_WriteOld(g_fb, true);  // OLD = inverse, so every pixel is driven
        EPD_SetRamOption(RAM_NORMAL, RAM_NORMAL);
        uint32_t b = 0;
        EPD_Update(0xD7, &b);  // factory Mode 1 full refresh
      }
      Serial.printf("  cycle %d/%d\n", i + 1, n);
    }
    fb_fill(WHITE);
    EPD_WriteNew(g_fb);
    EPD_WriteOld(g_fb, false);
    memcpy(g_prev, g_fb, sizeof(g_prev));
    arm_wave();
    Serial.println("DEGHOST done — panel is clean white");
    return;
  }
  if (!strcmp(s, "white")) { fb_fill(WHITE); panel_seed(); Serial.println("OK white"); return; }
  if (!strcmp(s, "black")) { fb_fill(BLACK); panel_seed(); Serial.println("OK black"); return; }

  if (!strncmp(s, "custom ", 7)) {
    g_custom = atoi(s + 7) != 0;
    if (g_custom) arm_wave();
    Serial.printf("OK custom=%u seq=0x%02X\n", (unsigned)g_custom, (unsigned)eff_seq());
    return;
  }
  if (!strncmp(s, "seq", 3) && (s[3] == ' ' || !s[3])) {
    if (s[3]) {
      if (!strncmp(s + 4, "auto", 4)) g_seq = 0;
      else { long v; if (parse_val(s + 4, &v)) g_seq = (uint8_t)v; }
    }
    uint8_t q = eff_seq();
    Serial.printf("SEQ 0x%02X clk=%u analog=%u temp=%u lut=%u mode2=%u display=%u "
                  "off_a=%u off_c=%u rails=%u\n",
                  (unsigned)q, (unsigned)!!(q & 0x80), (unsigned)!!(q & 0x40),
                  (unsigned)!!(q & 0x20), (unsigned)!!(q & 0x10), (unsigned)!!(q & 0x08),
                  (unsigned)!!(q & 0x04), (unsigned)!!(q & 0x02), (unsigned)!!(q & 0x01),
                  (unsigned)EPD_AnalogIsOn());
    return;
  }
  // `drive N` is the mode dial, so it clears and reseeds before running — same as
  // the 5.79 sibling. A mode has to be judged on a BLANK panel: switching straight
  // into it leaves the previous mode's ghosting on the glass, and that ghosting
  // then reads as the new mode's trail, which is backwards when the fast modes are
  // the ones under suspicion. Clearing afterwards instead just destroys the
  // evidence. panel_seed() re-arms, because the vendor sequences reload the OTP LUT.
  if (!strncmp(s, "drive ", 6)) {
    long v;
    if (parse_val(s + 6, &v) && v >= 1 && v <= 255) {
      g_wp.drive_tp = (uint8_t)v;
      arm_wave();
      panel_clear_white();
      render();
      panel_seed();
    }
    Serial.printf("OK drive=%u (cleared + reseeded)\n", (unsigned)g_wp.drive_tp);
    return;
  }
  if (!strncmp(s, "settle ", 7)) {
    long v; if (parse_val(s + 7, &v) && v >= 0 && v <= 255) { g_wp.settle_tp = (uint8_t)v; arm_wave(); }
    Serial.printf("OK settle=%u\n", (unsigned)g_wp.settle_tp); return;
  }
  if (!strncmp(s, "rp ", 3)) {
    long v; if (parse_val(s + 3, &v) && v >= 1 && v <= 255) { g_wp.rp = (uint8_t)v; arm_wave(); }
    Serial.printf("OK rp=%u\n", (unsigned)g_wp.rp); return;
  }
  if (!strncmp(s, "fr ", 3)) {
    long v; if (parse_val(s + 3, &v) && v >= 0 && v <= 255) { g_wp.fr = (uint8_t)v; arm_wave(); }
    Serial.printf("OK fr=0x%02X\n", (unsigned)g_wp.fr); return;
  }
  if (!strncmp(s, "pol ", 4)) {
    g_wp.invert = atoi(s + 4) != 0; arm_wave();
    Serial.printf("OK pol=%u\n", (unsigned)g_wp.invert); return;
  }
  if (!strncmp(s, "spi", 3)) {
    const char *a = s + 3;
    while (*a == ' ') a++;
    if (!strncmp(a, "bitbang", 7)) EPD_SetSpiPath(EPD_SPI_BITBANG);
    else if (!strncmp(a, "hw", 2)) {
      // Ceiling is 20, the SSD1677's specified maximum, and TESTED: overclocking
      // does not work on this panel. The ESP32 divides 80 MHz by an integer, so
      // the only options above 20 (80/4) are 26.7 (80/3) and 40 (80/2), and both
      // wedge the controller — transfers still complete and busy_ms goes to 0,
      // meaning it stops acting on commands entirely. Worse, it stays wedged: the
      // RST pulse in EPD_InitFast() does NOT clear it, only a full board reset
      // does. Raise this only with better wiring, and expect to power-cycle.
      int mhz = atoi(a + 2);
      if (mhz >= 1 && mhz <= 20) EPD_SetHwSpiHz((uint32_t)mhz * 1000000UL);
      EPD_SetSpiPath(EPD_SPI_HW);
    }
    // Re-initialise the panel after ANY change of transport. Rebuilding the SPI
    // device calls gpio_reset_pin() on SCK and MOSI, which floats the clock line
    // and clocks garbage into the controller; it then ignores every subsequent
    // command. The symptom is busy_ms=0 — transfers still complete, the panel
    // simply never refreshes — and it persisted after switching back to 20 MHz,
    // which is what showed the clock was never the problem.
    EPD_InitFast();
    arm_wave();
    Serial.printf("OK spi=%s (panel re-initialised)\n", EPD_SpiPathName());
    return;
  }
  Serial.printf("ERR unknown '%s'\n", s);
}

static void poll_serial(void)
{
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { g_line[g_len] = 0; handle_cmd(g_line); g_len = 0; continue; }
    if (g_len < sizeof(g_line) - 1) g_line[g_len++] = c;
  }
}

// ---- arduino ----------------------------------------------------------------
void setup(void)
{
  // Must precede begin(). The CDC receive queue defaults to 256 bytes, which is
  // fine for typed commands and useless for streaming video: a 4096-byte chunk
  // arrived as 1205 bytes, the rest silently dropped on overflow. Sized to hold
  // a whole chunk with room to spare so the USB task never has to discard.
  Serial.setRxBufferSize(65536);
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  // Never block the render loop on USB. Without this, a write with no host
  // reading stalls until the CDC timeout expires, once per frame.
  Serial.setTxTimeoutMs(0);
#endif
  delay(200);

  pinMode(EPD_BUSY, INPUT);
  // 20 MHz is the datasheet ceiling for writes on this part (p.5). These pins
  // are not the S3's SPI IOMUX pins, so they route via the GPIO matrix — fine
  // at 20 MHz, but there is no headroom above it anyway.
  // 40 MHz = 80/2, DOUBLE the SSD1677's specified 20, and it runs clean: SPI
  // drops from 39 ms to 20 ms per frame with BUSY unchanged at 129 ms.
  //
  // Set it HERE, at boot, and never at runtime. Changing the clock while the
  // panel is live wedges the controller — transfers still complete but it stops
  // acting on commands, with busy_ms=0 the only symptom, and only a full board
  // reset clears it. That is a property of rebuilding the SPI device mid-traffic,
  // NOT of the clock: 26.7 and 40 MHz both wedged the panel when switched into at
  // runtime, and both are stable when the controller only ever sees one speed.
  // That distinction is why `spi hw N` stays clamped to 20 — it is a runtime path.
  EPD_SetHwSpiHz(40000000);
  EPD_SetSpiPath(EPD_SPI_HW);
  EPD_GPIOInit();

  Serial.println();
  Serial.println("=== Seeed 4.26\" 800x480 / GDEQ0426T82 / SSD1677 — bring-up ===");
  Serial.printf("pins RST=%d CS=%d BUSY=%d DC=%d SCK=%d MOSI=%d\n", EPD_RST, EPD_CS,
                EPD_BUSY, EPD_DC, EPD_SCK, EPD_MOSI);
  Serial.printf("framebuffer %lu bytes x2\n", (unsigned long)EPD_PLANE_BYTES);

  wave1677_defaults(&g_wp);

  // Source rails at the datasheet maximum, in FIRMWARE rather than only in
  // play.py. VSH1 15 -> 17 V and VSL -15 -> -17 V is ~13% more drive per frame at
  // an identical frame count, so no mode loses any time for it.
  //
  // This used to be applied host-side, which meant the panel silently reverted to
  // stock +/-15 V on every reset — so anything driven WITHOUT play.py (the game,
  // the bench demo, a bare boot) ran at lower contrast than the measurements in the
  // README were taken at. That is a plausible part of why this panel reads so much
  // lighter than the 5.79 at the same nominal drive count; the rest is gate count,
  // since impulse per pixel per frame goes as frame_period / gates and this panel
  // has 480 gates to the 5.79's 272.
  //
  // Kept OUT of wave1677_defaults() on purpose: leaving that at power-on values is
  // what makes a custom LUT built from it a true phases-only experiment.
  g_wp.vsh1 = 0x4B;  // +17 V
  g_wp.vsl = 0x3A;   // -17 V

  EPD_InitFast();
  Serial.println("init done, clearing...");
  panel_clear_white();

  render();
  panel_seed();

  // Start on the fast custom waveform. This board has no buttons, so anything
  // that needs a serial command to come alive just looks broken on the bench.
  g_custom = true;
  arm_wave();

  print_help();
  // Do not boot LongTris. The webcam/host `stream` path needs the command
  // prompt immediately; the game loop ate serial as WASD and blocked stream.
  // `tetris` still starts it by hand.
  Serial.println("READY — bench. `stream` for video, `tetris` to play.");
}

void loop(void)
{
  poll_serial();

  if (g_cycle_ms && (millis() - g_cycle_at) >= g_cycle_ms) {
    g_cycle_at = millis();
    g_cycle_i = (uint8_t)((g_cycle_i + 1) % (sizeof(kCycle) / sizeof(kCycle[0])));
    g_wp.drive_tp = kCycle[g_cycle_i];
    arm_wave();
    Serial.printf("CYCLE drive=%u predicted_ms=%lu\n", (unsigned)g_wp.drive_tp,
                  (unsigned long)predicted_busy_ms());
  }

  if (!g_running && !g_step_once) { delay(5); return; }
  g_step_once = false;

  if (g_gray_anim) {
    gray_anim_frame();
    g_frame++;
    return;
  }

  if (g_dither_anim) {
    dither_anim_render();
  } else {
    advance();
    render();
  }
  show_frame();
  g_frame++;
}

