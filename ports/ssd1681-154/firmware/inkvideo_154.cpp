// inkvideo — Watchy 1.54" 200x200 SSD1681 (env xiao-154).
// Not ifdef'd into xiao-426. OTP waveform until a LUT is measured on glass.
// FPC vs Seeed 24-pin must be checked before plugging. See docs/WATCHY_154.md.

#include "board.h"
#include "spi.h"
#include "ssd1681.h"
#include "wave1681.h"
#include "stream_wire.h"

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <soc/rtc_cntl_reg.h>
#include <string.h>

#define ACK STREAM_ACK
#define CAN STREAM_CAN

// 0x22 Display Update Control 2, MSB first:
//   b7 clock on   b6 analog on   b5 load temperature   b4 load LUT mode 1
//   b3 load LUT mode 2   b2 display   b1 analog off   b0 clock off
//
// b4 is the LUT *load* enable; b3 only selects which LUT gets loaded. That is
// the correction: SEQ_MODE2 used to be 0x0C, copied off the SSD1677, and 0x0C
// has b3 without b4 — so it loaded nothing and displayed with whatever LUT was
// already resident. Measured on this glass, `blink` busy_ms per frame:
//
//   0xF7  2132 ms  mode-1 full refresh, loads temperature and its own rails
//   0xFC   458 ms  b7..b2: clock + analog + temp + load LUT + mode 2 + display
//   0x1C   363 ms  load LUT + mode 2 + display. Rails and clock already up,
//                  no temperature read. Self-sufficient and the fastest.
//   0x0C   362 ms  *only* once a mode-2 LUT is resident; straight after a full
//                  refresh it re-runs the mode-1 LUT at 1895 ms instead.
//
// So 0x1C is the default: same speed as 0x0C without depending on what ran
// before it. `seq X` walks any of them with no reflash.
#define SEQ_FULL 0xF7
#define SEQ_PART 0xFC
#define SEQ_FAST 0x1C
#define SEQ_RESIDENT 0x0C
#ifndef DISP_BORDER_W
#define DISP_BORDER_W 1
#endif

static uint8_t *g_prev = nullptr;
static uint8_t *g_sbuf[2] = {nullptr, nullptr};
static uint8_t *g_bmp = nullptr;
static uint8_t *g_lit = nullptr;

// 24 slots x 5000 bytes = 120 KB of PSRAM. Raised from 12 so `bb` can reach
// 20 without hist_or() silently clamping the depth it was asked for.
#define TRAIL_MAX 24
static uint8_t *g_hist[TRAIL_MAX];
static uint8_t *g_trail_mask = nullptr;
static uint8_t *g_bb_mask = nullptr;
static uint8_t g_hist_pos = 0;
static uint8_t g_trail = 7;
static uint8_t g_trail_every = 2;
static uint32_t g_stream_frame = 0;
// Per-frame cost accounting. The 5.79" spent a round auditing its SPI path
// because the mask work sat inside the SPI timing span and was charged to it.
// Time the two separately or the same mistake is available here.
static uint32_t g_us_mask = 0, g_us_spi = 0, g_us_busy = 0;
static uint8_t g_drive = 2;
static uint8_t g_seq2 = SEQ_FAST;
// 0x3F. POR 0x22 costs +40.25 ms per update in discharge scan frames; 0x07
// removes it with the slope untouched. Default to the measured-good value.
static uint8_t g_eopt = 0x07;
static Wave1681 g_wp;
// ON by default. Measured on glass: drive 4 renders the test card at full
// contrast in 40 ms against the OTP partial's 363 ms. The OTP path is still
// there behind `fast off`.
static bool g_wave_on = true;
static uint8_t g_lut[WAVE1681_BYTES];
// Frame 0 of a stream is a delta against white on both ends of the wire.
// Track whether that is actually true of the glass instead of trusting it.
static bool g_white_known = false;

static SemaphoreHandle_t g_sem_free = nullptr;
static SemaphoreHandle_t g_sem_ready = nullptr;
static volatile bool g_srx_run = false;
static volatile bool g_srx_fail = false;

static char g_line[96];
static uint8_t g_len = 0;

static void trail_reset(void) {
  for (uint8_t k = 0; k < TRAIL_MAX; k++)
    if (g_hist[k]) memset(g_hist[k], 0, EPD_PLANE_BYTES);
  if (g_trail_mask) memset(g_trail_mask, 0, EPD_PLANE_BYTES);
  if (g_bb_mask) memset(g_bb_mask, 0, EPD_PLANE_BYTES);
  g_hist_pos = 0;
}

// Push this frame's change map into the ring.
static void hist_push(const uint8_t *cur, const uint8_t *prev) {
  uint8_t *slot = g_hist[g_hist_pos];
  if (!slot) return;
  const uint32_t nw = EPD_PLANE_BYTES / 4u;
  const uint32_t *a = (const uint32_t *)cur;
  const uint32_t *b = (const uint32_t *)prev;
  uint32_t *d = (uint32_t *)slot;
  for (uint32_t i = 0; i < nw; i++) d[i] = a[i] ^ b[i];
  g_hist_pos = (uint8_t)((g_hist_pos + 1) % TRAIL_MAX);
}

// OR the most recent n change maps into dst.
static void hist_or(uint8_t *dst, uint8_t n) {
  if (!dst) return;
  const uint32_t nw = EPD_PLANE_BYTES / 4u;
  if (n > TRAIL_MAX) n = TRAIL_MAX;
  if (n < 1) n = 1;
  memcpy(dst, g_hist[(g_hist_pos + TRAIL_MAX - 1) % TRAIL_MAX],
         EPD_PLANE_BYTES);
  for (uint8_t k = 1; k < n; k++) {
    const uint32_t *h =
        (const uint32_t *)g_hist[(g_hist_pos + TRAIL_MAX - 1 - k) % TRAIL_MAX];
    if (!h) continue;
    uint32_t *m = (uint32_t *)dst;
    for (uint32_t i = 0; i < nw; i++) m[i] |= h[i];
  }
}

// The top-up mask: pixels that changed within the last bb_n frames and are
// BLACK now, plus those that changed within ww_n frames and are WHITE now.
//
// Writing OLD = NEW ^ mask makes the controller classify those pixels as
// changed and run the full drive on them again, so a pixel that has just
// reached its colour gets n extra drives and then stops -- it ages out of the
// ring and is never re-driven until it changes again. That is the whole
// difference from trail/chase, which keeps re-driving everything in history
// regardless of whether it settled, and pulls the field dark.
//
// Black is 0 in the plane and white is 1, hence `& ~cur` and `& cur`. The two
// depths are independent, but there is only ONE OLD plane write per frame, so
// they have to be combined here rather than issued separately.
//
// g_trail_mask is borrowed as scratch: trail/chase is bypassed whenever either
// top-up is on.
static void topup_mask_build(const uint8_t *cur, uint8_t bb_n, uint8_t ww_n) {
  if (!g_bb_mask || !g_trail_mask) return;
  const uint32_t nw = EPD_PLANE_BYTES / 4u;
  uint32_t *m = (uint32_t *)g_bb_mask;
  const uint32_t *c = (const uint32_t *)cur;
  memset(g_bb_mask, 0, EPD_PLANE_BYTES);
  if (bb_n) {
    hist_or(g_trail_mask, bb_n);
    const uint32_t *t = (const uint32_t *)g_trail_mask;
    for (uint32_t i = 0; i < nw; i++) m[i] |= t[i] & ~c[i];
  }
  if (ww_n) {
    hist_or(g_trail_mask, ww_n);
    const uint32_t *t = (const uint32_t *)g_trail_mask;
    for (uint32_t i = 0; i < nw; i++) m[i] |= t[i] & c[i];
  }
}

static void park_rim(uint8_t *p) {
  memset(p, 0xFF, EPD_WBYTES);
  memset(p + (EPD_H - 1) * EPD_WBYTES, 0xFF, EPD_WBYTES);
  for (uint16_t y = 0; y < EPD_H; y++) {
    p[y * EPD_WBYTES] |= 0x80;
    p[y * EPD_WBYTES + EPD_WBYTES - 1] |= 0x01;
  }
}

// Re-armed before EVERY custom-waveform drive. 153 bytes at 20 MHz is 61 us,
// so there is no reason to track whether something clobbered it: any activate
// byte carrying b4 reloads the OTP LUT, and SEQ_FULL (0xF7) does exactly that
// on every `white`. Cheaper to rewrite than to reason about.
static void arm_wave(void) {
  wave1681_build(g_lut, &g_wp);
  EPD_WriteLut(g_lut);
  EPD_WriteEopt(g_eopt);
  EPD_WriteVoltages(0, g_wp.vsh1, g_wp.vsh2, g_wp.vsl, g_wp.vcom);
}

static uint16_t wave_frames(void) {
  // Repeats are n+1; the builder leaves SRAB and SRCD at 0. The BB window is
  // carved out of the drive phase, so it does not appear here.
  return (uint16_t)((g_wp.rp + 1u) *
                    (g_wp.reset_tp + g_wp.drive_tp + g_wp.settle_tp));
}

// busy = frames x period, measured. Period is 8063 us at FR 0x05 and scales as
// 1/FR with a consistent 0.8% clock offset. There is no extra frame and no
// fixed cost once EOPT is 0x07 and the activate byte is rails-up.
static uint32_t wave_predict_us(void) {
  // All measured on this glass with `calib`. Every code lands 0.8 % low
  // against nominal -- one clock offset, not scatter.
  static const uint32_t PER[13] = {
      0, 40321, 20159, 13437, 10080, 8063, 0, 0, 0, 26862, 16118, 11508, 8953};
  const uint8_t fr = wv_fr_valid(g_wp.fr) ? g_wp.fr : 5;
  return (uint32_t)wave_frames() * PER[fr];
}

static void wave_show(void) {
  const uint32_t us = wave_predict_us();
  Serial.printf("FAST %s fr=0x%02X reset=%u drive=%u settle=%u bb=%u ww=%u "
                "rp=%u inv=%u map=%u/%u\n",
                g_wave_on ? "on" : "off", (unsigned)g_wp.fr,
                (unsigned)g_wp.reset_tp, (unsigned)g_wp.drive_tp,
                (unsigned)g_wp.settle_tp, (unsigned)g_wp.bb_tp,
                (unsigned)g_wp.ww_tp, (unsigned)g_wp.rp, (unsigned)g_wp.invert,
                (unsigned)g_wp.blk_black, (unsigned)g_wp.blk_white);
  if (g_wp.bb_tp || g_wp.ww_tp)
    Serial.printf("     top-up bb=%u ww=%u frames after a pixel changes, then"
                  " left alone. Overrides trail/chase.\n",
                  (unsigned)g_wp.bb_tp, (unsigned)g_wp.ww_tp);
  Serial.printf("     vsh1=0x%02X vsh2=0x%02X vsl=0x%02X vcom=0x%02X eopt=0x%02X\n",
                (unsigned)g_wp.vsh1, (unsigned)g_wp.vsh2, (unsigned)g_wp.vsl,
                (unsigned)g_wp.vcom, (unsigned)g_eopt);
  Serial.printf("     frames=%u predict=%lu us (%lu.%lu fps at +4ms SPI)\n",
                (unsigned)wave_frames(), (unsigned long)us,
                (unsigned long)(1000000UL / (us + 4000UL)),
                (unsigned long)((10000000UL / (us + 4000UL)) % 10));
}

static void drive_solid(uint8_t val) {
  memset(g_prev, val, EPD_PLANE_BYTES);
  if (val == 0xFF) park_rim(g_prev);
  EPD_SetRamOption(RAM_NORMAL, RAM_NORMAL);
  EPD_WriteNew(g_prev);
  EPD_WriteOld(g_prev, true);
  uint32_t b = 0;
  EPD_Update(SEQ_FULL, &b);
  g_white_known = false;
}

static void panel_white(void) {
  Serial.println("RESET 1 B/W cycles");
  drive_solid(0x00);
  drive_solid(0xFF);
  Serial.println("  cycle 1/1");
  memset(g_prev, 0xFF, EPD_PLANE_BYTES);
  park_rim(g_prev);
  EPD_WriteNew(g_prev);
  EPD_WriteOld(g_prev, false);
  if (!EPD_AnalogIsOn()) EPD_AnalogPower(true);
  g_white_known = true;
  trail_reset();
  Serial.println("OK white");
}

// One full black/white cycle per count. Standard deghost after a run of
// partials — the 426 build has had this; this one did not.
static void panel_scrub(uint8_t cycles) {
  if (cycles < 1) cycles = 1;
  if (cycles > 10) cycles = 10;
  Serial.printf("SCRUB %u B/W cycles\n", (unsigned)cycles);
  for (uint8_t i = 0; i < cycles; i++) {
    drive_solid(0x00);
    drive_solid(0xFF);
    Serial.printf("  cycle %u/%u\n", (unsigned)(i + 1), (unsigned)cycles);
  }
  memset(g_prev, 0xFF, EPD_PLANE_BYTES);
  park_rim(g_prev);
  EPD_WriteNew(g_prev);
  EPD_WriteOld(g_prev, false);
  g_white_known = true;
  trail_reset();
  Serial.printf("OK scrub %u cycles\n", (unsigned)cycles);
}

static inline void px(uint8_t *p, int x, int y, bool ink) {
  if (x < 0 || y < 0 || x >= EPD_W || y >= EPD_H) return;
  uint8_t *b = p + (uint32_t)y * EPD_WBYTES + (x >> 3);
  const uint8_t m = (uint8_t)(0x80u >> (x & 7));
  *b = ink ? (uint8_t)(*b & ~m) : (uint8_t)(*b | m);
}

static void fill_rect(uint8_t *p, int x0, int y0, int x1, int y1, bool ink) {
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++) px(p, x, y, ink);
}

// Device-side test card. Nothing in it touches the host, the wire or the
// encoder, so if this lands and a streamed frame does not, the fault is
// upstream of the panel. 8 px checkers show byte alignment; the diagonals
// break visibly on any row-stride or address-wrap error; the centre bar is the
// only flat area, so waveform banding shows up there first.
static void render_card(uint8_t *p, bool inverse) {
  memset(p, inverse ? 0x00 : 0xFF, EPD_PLANE_BYTES);
  const bool ink = !inverse;

  for (int t = 0; t < 3; t++) {
    fill_rect(p, t, t, EPD_W - 1 - t, t, ink);
    fill_rect(p, t, EPD_H - 1 - t, EPD_W - 1 - t, EPD_H - 1 - t, ink);
    fill_rect(p, t, t, t, EPD_H - 1 - t, ink);
    fill_rect(p, EPD_W - 1 - t, t, EPD_W - 1 - t, EPD_H - 1 - t, ink);
  }
  const int corner[2][2] = {{12, 12}, {EPD_W - 44, EPD_H - 44}};
  for (int c = 0; c < 2; c++)
    for (int a = 0; a < 4; a++)
      for (int b = 0; b < 4; b++)
        if (((a + b) & 1) == 0)
          fill_rect(p, corner[c][0] + a * 8, corner[c][1] + b * 8,
                    corner[c][0] + a * 8 + 7, corner[c][1] + b * 8 + 7, ink);
  fill_rect(p, 20, EPD_H / 2 - 14, EPD_W - 21, EPD_H / 2 + 14, ink);
  for (int x = 0; x < EPD_W; x++) {
    const int d = x * 40 / EPD_W;
    px(p, x, 60 + d, ink);
    px(p, x, 61 + d, ink);
    px(p, x, 140 - d, ink);
    px(p, x, 141 - d, ink);
  }
  park_rim(p);
}

static void panel_card(bool inverse) {
  render_card(g_prev, inverse);
  EPD_SetRamOption(RAM_NORMAL, RAM_NORMAL);
  EPD_WriteNew(g_prev);
  EPD_WriteOld(g_prev, true);
  uint32_t b = 0;
  uint32_t us = EPD_Update(SEQ_FULL, &b);
  g_white_known = false;
  trail_reset();
  Serial.printf("OK card%s busy=%lums total=%lums to=%u\n",
                inverse ? " inverse" : "", (unsigned long)(b / 1000),
                (unsigned long)(us / 1000), (unsigned)EPD_BusyTimedOut());
}

// Alternate card and inverse using the *mode 2* sequence, host uninvolved.
// This is the one test that answers "does g_seq2 move ink on this glass" —
// busy_ms per frame separates "waveform ran" from "waveform never started".
static void panel_blink(uint8_t n) {
  if (!g_sbuf[0] || !g_prev) {
    Serial.println("ERR no PSRAM");
    return;
  }
  if (n < 1) n = 1;
  if (n > 20) n = 20;
  if (!EPD_AnalogIsOn()) EPD_AnalogPower(true);
  // With the custom waveform armed the activate byte MUST NOT carry b4, or the
  // OTP LUT is reloaded and a different waveform gets timed.
  const uint8_t act = g_wave_on ? SEQ_RESIDENT : g_seq2;
  Serial.printf("BLINK %u frames seq=0x%02X%s\n", (unsigned)n, (unsigned)act,
                g_wave_on ? " custom" : " otp");
  if (g_wave_on) Serial.printf("  predict %lu us/frame\n",
                               (unsigned long)wave_predict_us());
  for (uint8_t i = 0; i < n; i++) {
    render_card(g_sbuf[0], (i & 1) != 0);
    if (g_wave_on) arm_wave();
    EPD_SetRamOption(RAM_NORMAL, RAM_NORMAL);
    EPD_WriteNew(g_sbuf[0]);
    EPD_WriteOld(g_prev, false);
    uint32_t b = 0;
    uint32_t us = EPD_Update(act, &b);
    memcpy(g_prev, g_sbuf[0], EPD_PLANE_BYTES);
    Serial.printf("  %u busy=%lums total=%lums to=%u\n", (unsigned)i,
                  (unsigned long)(b / 1000), (unsigned long)(us / 1000),
                  (unsigned)EPD_BusyTimedOut());
  }
  g_white_known = false;
  trail_reset();
  Serial.println("OK blink");
}

// One full refresh to white from whatever is on the glass. The black/white
// cycle in panel_white() is a deghost; stream entry only needs a known base,
// and paying a black flash for it on every rearm is not worth it.
static void ensure_white(void) {
  if (g_white_known) return;
  EPD_SetRamOption(RAM_NORMAL, RAM_NORMAL);
  EPD_WriteOld(g_prev, false);  // OLD = what is actually on the glass
  memset(g_prev, 0xFF, EPD_PLANE_BYTES);
  park_rim(g_prev);
  EPD_WriteNew(g_prev);
  uint32_t b = 0;
  uint32_t us = EPD_Update(SEQ_FULL, &b);
  g_white_known = true;
  trail_reset();
  Serial.printf("OK white1 busy=%lums total=%lums to=%u\n",
                (unsigned long)(b / 1000), (unsigned long)(us / 1000),
                (unsigned)EPD_BusyTimedOut());
}

// Non-destructive timing probe. `busy = fixed + frames x frame_period` is the
// law that made the 5.79" go 336 ms -> 89 ms; nothing has ever measured either
// term on this part. A null LUT (every VS phase at VSS) burns the full time
// cost with ZERO field across the pixel, so this cannot mark the glass.
//
// The 7-byte group order is unsettled on the SSD1681 — the SSD1680 figure reads
// TP-first, the SSD1683 only decodes RP-first. Under the wrong order the probe
// group decodes to RP = 0, every group is skipped, and BUSY goes FLAT against
// TP. So both are swept and the one with a slope is the right one. That is
// cheaper and more honest than picking from a datasheet figure.
//
// act must NOT carry b4 (load LUT from OTP) or the custom table is discarded
// before it runs — that is the SSD1683's 0xDC trap. 0x0C is display-only with
// the rails already up.
static void calib_one(uint8_t fr, uint8_t order, uint8_t act) {
  static const uint8_t TPS[4] = {4, 8, 16, 32};
  if (act & 0x10) {
    Serial.println("  refusing: act has b4, it would reload the OTP LUT");
    return;
  }
  Serial.printf("CALIB fr=0x%02X order=%s act=0x%02X eopt=0x%02X\n",
                (unsigned)fr, order ? "RP-first" : "TP-first", (unsigned)act,
                (unsigned)g_eopt);
  EPD_WriteEopt(g_eopt);
  if (!EPD_AnalogIsOn()) EPD_AnalogPower(true);

  uint32_t us[4];
  for (uint8_t i = 0; i < 4; i++) {
    wave1681_build_null(g_lut, TPS[i], fr, order);
    EPD_WriteLut(g_lut);
    uint32_t busy = 0;
    (void)EPD_Update(act, &busy);
    us[i] = busy;
    Serial.printf("  tp=%3u frames=%3u busy=%7lu us to=%u\n", (unsigned)TPS[i],
                  (unsigned)wave1681_frames(g_lut, order),
                  (unsigned long)busy, (unsigned)EPD_BusyTimedOut());
    if (EPD_BusyTimedOut()) {
      Serial.println("  abort: BUSY never released");
      return;
    }
  }

  // Least squares over the four points, integer maths — no float printf.
  int32_t sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (uint8_t i = 0; i < 4; i++) {
    const int32_t x = TPS[i], y = (int32_t)(us[i] / 1);
    sx += x;
    sy += y;
    sxx += x * x;
    sxy += x * y;
  }
  const int32_t den = 4 * sxx - sx * sx;
  const int32_t slope = den ? (4 * sxy - sx * sy) / den : 0;
  const int32_t fixed = (sy - slope * sx) / 4;
  Serial.printf("  => slope=%ld us/frame  fixed=%ld us  %s\n", (long)slope,
                (long)fixed,
                slope > 200 ? "TRACKS - this order decodes" : "FLAT");
  if (slope > 200)
    Serial.printf("     %ld frames would be %ld ms; 200-gate frame at %ld us\n",
                  (long)11, (long)(11 * slope / 1000), (long)slope);
}

static void wave_calib(uint8_t fr, uint8_t act) {
  // RP-first is settled: 1/slope came back 24.8/49.6/74.4/99.2/124.0 Hz against
  // nominal 25/50/75/100/125, and TP-first gave exactly a quarter of that at
  // every code. Only sweep the order again if the layout is in doubt.
  calib_one(fr, WAVE1681_ORDER_RP_FIRST, act);
  Serial.println("CALIB done. Resident LUT is the null probe: it drives nothing.");
  Serial.println("  `white`, or any seq with b4 (0x1C/0xFC), reloads OTP.");
  g_white_known = false;
}

// Same null LUT, fixed frame count, only the 0x22 activate byte varies. The
// difference between rows is entirely power sequencing and display-off cost --
// which is where the FR-independent 40.27 ms has to live, since 0x0C is already
// a rails-up byte. Bytes carrying b4 are skipped: they reload the OTP LUT and
// would be timing a different waveform.
// One group-0 timing byte at a time, everything else at a baseline that is
// known to measure frames = tp. A byte that multiplies is RP or an SR; a byte
// that adds is a TP; a byte that does nothing is a phase whose sub-group is
// switched off. Still a null LUT: every VS phase is VSS, nothing is driven.
static void wave_bytescan(void) {
  static const uint8_t BASE[7] = {1, 8, 0, 0, 0, 1, 0};
  const uint8_t P = 4;
  const uint32_t period = 8063;  // FR 0x05, measured
  Serial.println("BYTESCAN group-0, null LUT (drives nothing), fr=0x05");
  Serial.printf("  baseline = %u,%u,%u,%u,%u,%u,%u  probe value %u\n",
                (unsigned)BASE[0], (unsigned)BASE[1], (unsigned)BASE[2],
                (unsigned)BASE[3], (unsigned)BASE[4], (unsigned)BASE[5],
                (unsigned)BASE[6], (unsigned)P);
  EPD_WriteEopt(0x07);
  if (!EPD_AnalogIsOn()) EPD_AnalogPower(true);

  for (int8_t j = -1; j < 7; j++) {
    memset(g_lut, 0, WAVE1681_BYTES);
    uint8_t *g = g_lut + 60;
    for (uint8_t k = 0; k < 7; k++) g[k] = BASE[k];
    if (j >= 0) g[j] = P;
    for (uint8_t i = 0; i < 6; i++) g_lut[144 + i] = 0x55;
    EPD_WriteLut(g_lut);
    uint32_t b = 0;
    (void)EPD_Update(SEQ_RESIDENT, &b);
    const long fr = (long)((b + period / 2) / period) - 1;
    if (j < 0)
      Serial.printf("  baseline    busy=%7lu us  frames=%ld\n",
                    (unsigned long)b, fr);
    else
      Serial.printf("  byte[%d]=%u   busy=%7lu us  frames=%ld\n", (int)j,
                    (unsigned)P, (unsigned long)b, fr);
    if (EPD_BusyTimedOut()) {
      Serial.println("  abort: BUSY never released");
      return;
    }
  }
  Serial.println("BYTESCAN done");
  g_white_known = false;
}

static void wave_seqscan(uint8_t tp, uint8_t fr) {
  static const uint8_t ACTS[] = {0x04, 0x0C, 0x84, 0x8C, 0xC4, 0xCC, 0xCF};
  Serial.printf("SEQSCAN tp=%u fr=0x%02X eopt=0x%02X\n", (unsigned)tp,
                (unsigned)fr, (unsigned)g_eopt);
  EPD_WriteEopt(g_eopt);
  if (!EPD_AnalogIsOn()) EPD_AnalogPower(true);
  for (uint8_t i = 0; i < sizeof(ACTS); i++) {
    const uint8_t act = ACTS[i];
    wave1681_build_null(g_lut, tp, fr, WAVE1681_ORDER_RP_FIRST);
    EPD_WriteLut(g_lut);
    uint32_t busy = 0;
    (void)EPD_Update(act, &busy);
    Serial.printf("  act=0x%02X busy=%7lu us to=%u\n", (unsigned)act,
                  (unsigned long)busy, (unsigned)EPD_BusyTimedOut());
    if (!EPD_AnalogIsOn()) EPD_AnalogPower(true);  // 0xCF ramps down
  }
  Serial.println("SEQSCAN done (null LUT: nothing was driven)");
  g_white_known = false;
}

static bool parse_val(const char *s, long *out) {
  while (*s == ' ') s++;
  if (!*s) return false;
  char *end = nullptr;
  long v = (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
               ? strtol(s + 2, &end, 16)
               : strtol(s, &end, 10);
  if (end == s) return false;
  *out = v;
  return true;
}

// Two bare-hex bytes, as `volts 4B 3A` and the docs write them.
static bool parse_hex2(const char *s, long *a, long *b) {
  char *end = nullptr;
  while (*s == ' ') s++;
  *a = strtol(s, &end, 16);
  if (end == s) return false;
  s = end;
  while (*s == ' ') s++;
  if (!*s) return false;
  const char *s2 = s;
  *b = strtol(s2, &end, 16);
  return end != s2;
}

static bool stream_recv(uint8_t *dst, const uint8_t *base) {
  return stream_recv_wire2(dst, base, g_bmp, g_lit);
}

// Take exactly one wire=2 frame and apply it with the proven full refresh.
// It shares framing with the stream loop, so a frame that lands here but not
// there isolates the fault to the mode-2 sequence rather than to the wire.
static void frame_once(void) {
  if (!g_sbuf[0] || !g_bmp || !g_lit || !g_prev) {
    Serial.println("ERR no PSRAM");
    return;
  }
  stream_wire_reset();
  Serial.printf("FRAME ready wire=%u panel=xiao-154 200x200 seq=0x%02X\n",
                (unsigned)STREAM_WIRE, (unsigned)SEQ_FULL);
  Serial.flush();
  delay(20);
  if (!stream_recv_wire2(g_sbuf[0], g_prev, g_bmp, g_lit)) {
    Serial.println("FRAME abort");
    return;
  }
  EPD_SetRamOption(RAM_NORMAL, RAM_NORMAL);
  EPD_WriteNew(g_sbuf[0]);
  EPD_WriteOld(g_prev, false);
  uint32_t b = 0;
  uint32_t us = EPD_Update(SEQ_FULL, &b);
  memcpy(g_prev, g_sbuf[0], EPD_PLANE_BYTES);
  g_white_known = false;
  trail_reset();
  Serial.printf("FRAME done busy=%lums total=%lums to=%u\n",
                (unsigned long)(b / 1000), (unsigned long)(us / 1000),
                (unsigned)EPD_BusyTimedOut());
}

static void stream_rx_task(void *arg) {
  (void)arg;
  for (uint32_t i = 0;; i++) {
    if (xSemaphoreTake(g_sem_free, pdMS_TO_TICKS(200)) != pdTRUE) {
      if (!g_srx_run) break;
      i--;
      continue;
    }
    if (!g_srx_run) break;
    if (!stream_recv(g_sbuf[i & 1], g_sbuf[(i + 1) & 1])) {
      g_srx_fail = true;
      xSemaphoreGive(g_sem_ready);
      break;
    }
    xSemaphoreGive(g_sem_ready);
  }
  vTaskDelete(nullptr);
}

static void stream_run(void) {
  if (!g_sbuf[0] || !g_sbuf[1] || !g_bmp || !g_lit || !g_prev) {
    Serial.println("ERR no PSRAM");
    return;
  }
  // Seeding RAM to white without driving it makes every delta land on the
  // wrong base if the glass is holding a card or a previous run. Make white
  // true here rather than trusting the host to have sent `white` first.
  ensure_white();
  memset(g_sbuf[0], 0xFF, EPD_PLANE_BYTES);
  memset(g_sbuf[1], 0xFF, EPD_PLANE_BYTES);
  memset(g_prev, 0xFF, EPD_PLANE_BYTES);
  park_rim(g_sbuf[0]);
  park_rim(g_sbuf[1]);
  park_rim(g_prev);
  if (!EPD_AnalogIsOn()) EPD_AnalogPower(true);

  stream_wire_reset();
  g_srx_fail = false;
  g_srx_run = true;
  g_sem_free = xSemaphoreCreateCounting(2, 2);
  g_sem_ready = xSemaphoreCreateCounting(2, 0);

  trail_reset();
  g_stream_frame = 0;
  g_us_mask = g_us_spi = g_us_busy = 0;
  Serial.printf("STREAM ready wire=%u panel=xiao-154 200x200 seq=0x%02X "
                "trail=%u/%u drive=%u spi=%luHz %s\n",
                (unsigned)STREAM_WIRE,
                (unsigned)(g_wave_on ? SEQ_RESIDENT : g_seq2), (unsigned)g_trail,
                (unsigned)g_trail_every,
                (unsigned)(g_wave_on ? g_wp.drive_tp : g_drive),
                (unsigned long)EPD_GetHwSpiHz(),
                g_wave_on ? "custom-lut" : "otp-lut");
  Serial.flush();
  delay(20);

  TaskHandle_t rx = nullptr;
  xTaskCreatePinnedToCore(stream_rx_task, "srx", 4096, nullptr, 2, &rx, 0);

  uint32_t frames = 0, t0 = millis();
  bool pending = false;
  for (uint32_t c = 0;; c++) {
    if (xSemaphoreTake(g_sem_ready, pdMS_TO_TICKS(6000)) != pdTRUE) break;
    if (g_srx_fail) break;
    uint8_t *buf = g_sbuf[c & 1];
    if (pending) {
      const uint32_t t_b0 = micros();
      (void)EPD_WaitBusy_us();
      g_us_busy += micros() - t_b0;
    }
    const uint32_t t_new0 = micros();
    EPD_SetRamOption(RAM_NORMAL, RAM_NORMAL);
    EPD_WriteNew(buf);
    g_us_spi += micros() - t_new0;
    const bool topup = g_wp.bb_tp || g_wp.ww_tp;
    const uint32_t t_mask0 = micros();
    if (g_trail || topup) hist_push(buf, g_prev);
    const uint8_t *omask = nullptr;
    if (topup) {
      // The top-ups replace trail/chase rather than combining with it: only one
      // OLD plane write lands per frame, and the trail mask re-drives settled
      // pixels too, which is the thing being avoided.
      topup_mask_build(buf, g_wp.bb_tp, g_wp.ww_tp);
      omask = g_bb_mask;
    } else if (g_trail) {
      hist_or(g_trail_mask, g_trail);
      const bool chase =
          (g_trail_every == 1) ||
          (g_trail_every > 1 && (g_stream_frame % g_trail_every) == 0);
      if (chase) omask = g_trail_mask;
    }
    g_us_mask += micros() - t_mask0;
    const uint32_t t_spi0 = micros();
    if (omask) EPD_WriteOldMasked(buf, omask);
    g_us_spi += micros() - t_spi0;
    if (g_wave_on) arm_wave();
    EPD_UpdateKick(g_wave_on ? SEQ_RESIDENT : g_seq2);
    pending = true;
    memcpy(g_prev, buf, EPD_PLANE_BYTES);
    g_stream_frame++;
    frames++;
    xSemaphoreGive(g_sem_free);
  }
  if (pending) (void)EPD_WaitBusy_us();
  g_white_known = false;

  g_srx_run = false;
  xSemaphoreGive(g_sem_free);
  delay(200);
  vSemaphoreDelete(g_sem_free);
  vSemaphoreDelete(g_sem_ready);
  g_sem_free = g_sem_ready = nullptr;

  uint32_t ms = millis() - t0;
  if (!ms) ms = 1;
  Serial.printf("STREAM done %lu frames in %lu ms (%lu.%lu fps)\n",
                (unsigned long)frames, (unsigned long)ms,
                (unsigned long)(frames * 1000UL / ms),
                (unsigned long)((frames * 10000UL / ms) % 10));
  if (frames)
    Serial.printf("  per frame: busy=%lu us mask=%lu us spi=%lu us "
                  "other=%lu us\n",
                  (unsigned long)(g_us_busy / frames),
                  (unsigned long)(g_us_mask / frames),
                  (unsigned long)(g_us_spi / frames),
                  (unsigned long)((ms * 1000UL - g_us_busy - g_us_mask -
                                   g_us_spi) / frames));
}

static void print_help(void) {
  Serial.println("commands:");
  Serial.println("  help | status | reboot | download");
  Serial.println("  white | black | card | card inv | scrub N   full 0xF7 stills");
  Serial.println("  white1      one refresh to white, no black flash");
  Serial.println("  blink N     N mode-2 card flips, host uninvolved");
  Serial.println("  frame       one wire=2 frame, applied with the full 0xF7");
  Serial.println("  stream      wire=2 frames, applied with seq");
  Serial.println("  seq X       0x1C load LUT2 (363ms), 0xFC self-powered");
  Serial.println("  calib [fr]  null-LUT timing probe, marks nothing (fr 1..5)");
  Serial.println("  seqscan     same probe, sweeps the 0x22 activate byte");
  Serial.println("  bytescan    same probe, perturbs each group-0 timing byte");
  Serial.println("  eopt X      0x3F display-off. 0x22 POR = 2 discharge scans,");
  Serial.println("              0x07 = keep pixel voltage");
  Serial.println("  fast on|off custom LUT instead of the OTP waveform");
  Serial.println("  fast        show the table and its predicted busy");
  Serial.println("  drive N     target-polarity frames   (real when fast on)");
  Serial.println("  reset N     opposite-polarity kick. 0 when trail chasing.");
  Serial.println("  settle N    VSS hold frames");
  Serial.println("  bb N        0..20 extra drives for a pixel that just went");
  Serial.println("              black, then left alone. Free. Overrides trail.");
  Serial.println("  ww N        same for white. If it greys the field, raise");
  Serial.println("              the white rail: volts 4B 32");
  Serial.println("  fr N        frame-rate code. 1=25 2=50 3=75 4=100 5=125Hz,");
  Serial.println("              9=37.5 10=62.5 11=87.5 12=112.5. 6-8 are NA.");
  Serial.println("  rp N        group repeat");
  Serial.println("  inv 0|1     swap which rail drives toward black");
  Serial.println("  map A B     which LUT blocks are to-black / to-white");
  Serial.println("  vcom X      0x08 = -0.2V, the DC-creep fix");
  Serial.println("  volts A B   VSH1 VSL hex (POR 41 32 = +/-15V)");
  Serial.println("  trail N | chase N");
  Serial.println("  drive/volts/settle accepted, no custom LUT on glass yet");
}

static void go_download(void) {
  Serial.println("DOWNLOAD");
  Serial.flush();
  delay(80);
  REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
  esp_restart();
}

static void print_status(void) {
  Serial.printf("STATUS panel=xiao-154 200x200 wire=%u seq=0x%02X drive=%u "
                "trail=%u/%u spi=%s %luHz analog=%u white=%u otp-lut\n",
                (unsigned)STREAM_WIRE, (unsigned)g_seq2, (unsigned)g_drive,
                (unsigned)g_trail, (unsigned)g_trail_every, EPD_SpiPathName(),
                (unsigned long)EPD_GetHwSpiHz(), (unsigned)EPD_AnalogIsOn(),
                (unsigned)g_white_known);
  Serial.printf("       eopt=0x%02X\n", (unsigned)g_eopt);
  if (g_wave_on) wave_show();
}

static void handle_cmd(char *s) {
  while (*s == ' ') s++;
  if (!*s) return;
  if (!strcmp(s, "help")) {
    print_help();
    return;
  }
  if (!strcmp(s, "status")) {
    print_status();
    return;
  }
  if (!strcmp(s, "white") || !strcmp(s, "reset")) {
    panel_white();
    return;
  }
  if (!strcmp(s, "white1")) {
    g_white_known = false;
    ensure_white();
    return;
  }
  if (!strcmp(s, "black")) {
    drive_solid(0x00);
    Serial.println("OK black");
    return;
  }
  if (!strcmp(s, "card")) {
    panel_card(false);
    return;
  }
  if (!strcmp(s, "card inv") || !strcmp(s, "card inverse")) {
    panel_card(true);
    return;
  }
  if (!strcmp(s, "scrub")) {
    panel_scrub(3);
    return;
  }
  if (!strcmp(s, "blink")) {
    panel_blink(6);
    return;
  }
  if (!strcmp(s, "frame")) {
    frame_once();
    return;
  }
  if (!strcmp(s, "stream")) {
    stream_run();
    return;
  }
  if (!strcmp(s, "fast")) {
    wave_show();
    return;
  }
  if (!strcmp(s, "fast on")) {
    g_wave_on = true;
    arm_wave();
    wave_show();
    return;
  }
  if (!strcmp(s, "fast off")) {
    g_wave_on = false;
    Serial.println("OK fast off (next b4 sequence reloads OTP)");
    return;
  }
  if (!strcmp(s, "bytescan")) {
    wave_bytescan();
    return;
  }
  if (!strcmp(s, "seqscan")) {
    wave_seqscan(8, WV_FR_125);
    return;
  }
  if (!strcmp(s, "calib")) {
    wave_calib(WV_FR_125, SEQ_RESIDENT);
    return;
  }
  if (!strcmp(s, "reboot")) {
    Serial.println("REBOOT");
    Serial.flush();
    delay(50);
    esp_restart();
  }
  if (!strcmp(s, "download") || !strcmp(s, "dl")) {
    go_download();
    return;
  }
  long v;
  if (!strncmp(s, "scrub ", 6)) {
    panel_scrub(parse_val(s + 6, &v) && v > 0 ? (uint8_t)v : 3);
    return;
  }
  if (!strncmp(s, "blink ", 6)) {
    panel_blink(parse_val(s + 6, &v) && v > 0 ? (uint8_t)v : 6);
    return;
  }
  if (!strncmp(s, "calib ", 6)) {
    if (parse_val(s + 6, &v) && v >= 1 && v <= 0x0F)
      wave_calib((uint8_t)v, SEQ_RESIDENT);
    else
      Serial.println("ERR calib needs an FR code 1..15 (1=25Hz .. 5=125Hz)");
    return;
  }
  if (!strncmp(s, "eopt ", 5)) {
    if (parse_val(s + 5, &v) && v >= 0 && v <= 0xFF) g_eopt = (uint8_t)v;
    EPD_WriteEopt(g_eopt);
    Serial.printf("OK eopt=0x%02X%s\n", (unsigned)g_eopt,
                  g_eopt == 0x22 ? " POR, 2 discharge scan frames"
                                 : (g_eopt == 0x07 ? " keep pixel voltage" : ""));
    return;
  }
  if (!strncmp(s, "seq ", 4)) {
    // b2 is DISPLAY. Without it the sequence would load a LUT and drive
    // nothing, which looks exactly like a dead panel.
    if (parse_val(s + 4, &v) && v > 0 && v <= 0xFF && (v & 0x04))
      g_seq2 = (uint8_t)v;
    else
      Serial.println("  (need 1..0xFF with b2 DISPLAY set; try 0xFC or 0x0C)");
    Serial.printf("OK seq=0x%02X%s\n", (unsigned)g_seq2,
                  g_seq2 == SEQ_FAST
                      ? " load LUT2 + display, 363ms"
                      : (g_seq2 == SEQ_PART
                             ? " self-powered, 458ms"
                             : (g_seq2 == SEQ_RESIDENT
                                    ? " reuses the resident LUT — 1895ms if"
                                      " that is the full one"
                                    : "")));
    return;
  }
  if (!strncmp(s, "drive ", 6)) {
    if (parse_val(s + 6, &v) && v >= 1 && v <= 63) {
      g_wp.drive_tp = (uint8_t)v;
      g_drive = (uint8_t)v;
    }
    if (g_wave_on) {
      arm_wave();
      wave_show();
    } else {
      Serial.printf("OK drive=%u (fast off — OTP waveform ignores it)\n",
                    (unsigned)g_wp.drive_tp);
    }
    return;
  }
  if (!strncmp(s, "reset ", 6)) {
    if (parse_val(s + 6, &v) && v >= 0 && v <= 63) g_wp.reset_tp = (uint8_t)v;
    if (g_wp.reset_tp && g_trail)
      Serial.println("  warning: a non-zero reset replays as a dark pulse "
                     "behind trail-chased pixels");
    if (g_wave_on) arm_wave();
    wave_show();
    return;
  }
  if (!strncmp(s, "bb ", 3)) {
    // Kept under TRAIL_MAX so the ring always holds the requested depth.
    // Depth is NOT free in host time: hist_or() walks one 5000-byte slot per
    // frame of depth, and the STREAM done line reports mask= for exactly this.
    if (parse_val(s + 3, &v) && v >= 0 && v <= 20) g_wp.bb_tp = (uint8_t)v;
    else Serial.println("  bb takes 0..20 extra frames (0 = off)");
    if (g_wave_on) arm_wave();
    wave_show();
    return;
  }
  if (!strncmp(s, "ww ", 3)) {
    if (parse_val(s + 3, &v) && v >= 0 && v <= 20) g_wp.ww_tp = (uint8_t)v;
    else Serial.println("  ww takes 0..20 extra frames (0 = off)");
    if (g_wave_on) arm_wave();
    wave_show();
    return;
  }
  if (!strncmp(s, "settle ", 7)) {
    if (parse_val(s + 7, &v) && v >= 0 && v <= 63) g_wp.settle_tp = (uint8_t)v;
    if (g_wave_on) arm_wave();
    wave_show();
    return;
  }
  if (!strncmp(s, "fr ", 3)) {
    if (parse_val(s + 3, &v) && wv_fr_valid((uint8_t)v)) g_wp.fr = (uint8_t)v;
    else
      Serial.println("  1=25 2=50 3=75 4=100 5=125Hz, 9=37.5 10=62.5 11=87.5 "
                     "12=112.5Hz. 6/7/8 are NA on this part.");
    if (g_wave_on) arm_wave();
    wave_show();
    return;
  }
  if (!strncmp(s, "rp ", 3)) {
    if (parse_val(s + 3, &v) && v >= 0 && v <= 255) g_wp.rp = (uint8_t)v;
    if (g_wave_on) arm_wave();
    wave_show();
    return;
  }
  if (!strncmp(s, "inv ", 4)) {
    if (parse_val(s + 4, &v)) g_wp.invert = v != 0;
    if (g_wave_on) arm_wave();
    wave_show();
    return;
  }
  if (!strncmp(s, "map ", 4)) {
    long a = 0, b = 0;
    const char *p2 = s + 4;
    if (parse_val(p2, &a)) {
      while (*p2 && *p2 != ' ') p2++;
      if (parse_val(p2, &b) && a >= 0 && a <= 4 && b >= 0 && b <= 4 && a != b) {
        g_wp.blk_black = (uint8_t)a;
        g_wp.blk_white = (uint8_t)b;
      } else {
        Serial.println("  need two distinct block indices 0..4");
      }
    }
    if (g_wave_on) arm_wave();
    wave_show();
    return;
  }
  if (!strncmp(s, "vcom ", 5)) {
    if (parse_val(s + 5, &v) && v >= 0x08 && v <= 0x78) g_wp.vcom = (uint8_t)v;
    else Serial.println("  vcom is 0x08 (-0.2V) .. 0x78 (-3V); 0x00 is undefined");
    if (g_wave_on) arm_wave();
    wave_show();
    return;
  }
  if (!strncmp(s, "trail ", 6)) {
    if (parse_val(s + 6, &v) && v >= 0 && v <= TRAIL_MAX) {
      g_trail = (uint8_t)v;
      trail_reset();
    }
    Serial.printf("OK trail=%u\n", (unsigned)g_trail);
    return;
  }
  if (!strncmp(s, "chase ", 6)) {
    if (parse_val(s + 6, &v) && v >= 0 && v <= 8) g_trail_every = (uint8_t)v;
    Serial.printf("OK chase=%u\n", (unsigned)g_trail_every);
    return;
  }
  if (!strncmp(s, "volts ", 6)) {
    // VSH1 then VSL, hex, as the host sends them. In spec: VSH1 0x21..0x4B
    // (8.6..17 V), VSL 0x0A..0x3A (-5..-17 V). Refuse anything outside that.
    // The host and every doc write these as BARE hex: `volts 4B 3A`. parse_val
    // only treats an 0x prefix as hex, so it read 4B as decimal 4 and the
    // in-spec check then rejected a perfectly good value.
    long a = 0, b = 0;
    const char *p2 = s + 6;
    if (parse_hex2(p2, &a, &b)) {
      {
        if (a >= 0x21 && a <= 0x4B && b >= 0x0A && b <= 0x3A) {
          g_wp.vsh1 = (uint8_t)a;
          g_wp.vsl = (uint8_t)b;
          if (g_wave_on) arm_wave();
        } else {
          Serial.println("  out of spec: VSH1 0x21..0x4B, VSL 0x0A..0x3A");
        }
      }
    }
    Serial.printf("OK volts vsh1=0x%02X vsl=0x%02X%s\n", (unsigned)g_wp.vsh1,
                  (unsigned)g_wp.vsl, g_wave_on ? "" : " (fast off)");
    return;
  }
  Serial.printf("ERR unknown '%s'\n", s);
}

static void poll_serial(void) {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    // A CAN left over from a host that aborted a stream used to land in the
    // line buffer and turn the next command into `\x18status`.
    if (c != '\n' && ((unsigned char)c < 0x20 || (unsigned char)c == 0x7F))
      continue;
    if (c == '\n') {
      g_line[g_len] = 0;
      handle_cmd(g_line);
      g_len = 0;
      continue;
    }
    if (g_len < sizeof(g_line) - 1) g_line[g_len++] = c;
  }
}

void setup(void) {
  Serial.setRxBufferSize(65536);
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  // NOT 0. At 0 a full TX FIFO silently DROPS bytes, and that is what ate the
  // `STREAM ready` newline and then the host's first ACK. A small non-zero cap
  // makes banners arrive intact without ever letting a stalled reader hang the
  // panel loop — the worst case is one 20 ms stall per write.
  Serial.setTxTimeoutMs(20);
#endif
  delay(200);

  g_prev = (uint8_t *)heap_caps_malloc(EPD_PLANE_BYTES, MALLOC_CAP_SPIRAM);
  g_sbuf[0] = (uint8_t *)heap_caps_malloc(EPD_PLANE_BYTES, MALLOC_CAP_SPIRAM);
  g_sbuf[1] = (uint8_t *)heap_caps_malloc(EPD_PLANE_BYTES, MALLOC_CAP_SPIRAM);
  g_bmp = (uint8_t *)heap_caps_malloc(EPD_PLANE_BYTES / 8, MALLOC_CAP_SPIRAM);
  g_lit = (uint8_t *)heap_caps_malloc(EPD_PLANE_BYTES, MALLOC_CAP_SPIRAM);
  g_trail_mask = (uint8_t *)heap_caps_malloc(EPD_PLANE_BYTES, MALLOC_CAP_SPIRAM);
  g_bb_mask = (uint8_t *)heap_caps_malloc(EPD_PLANE_BYTES, MALLOC_CAP_SPIRAM);
  bool hist_ok = true;
  for (uint8_t k = 0; k < TRAIL_MAX; k++) {
    g_hist[k] = (uint8_t *)heap_caps_malloc(EPD_PLANE_BYTES, MALLOC_CAP_SPIRAM);
    if (!g_hist[k]) hist_ok = false;
  }
  if (!g_prev || !g_sbuf[0] || !g_sbuf[1] || !g_bmp || !g_lit || !g_trail_mask ||
      !g_bb_mask || !hist_ok) {
    Serial.println("FATAL no PSRAM");
    for (;;) delay(1000);
  }

  wave1681_defaults(&g_wp);
  EPD_SetHwSpiHz(20000000);
  EPD_SetSpiPath(EPD_SPI_HW);
  EPD_GPIOInit();
  EPD_Init();
  panel_white();
  // Arm before the first frame. EOPT and VCOM only take effect on the update
  // after they are written, so the very first custom drive after arming comes
  // out weak -- measured once as contrast 55 where every repeat gave 95.
  arm_wave();

  print_help();
  print_status();
  Serial.println("READY xiao-154 — OTP LUT, wire=2. Do not use on 4.26\".");
  Serial.println("  bring-up order: card -> blink -> frame -> stream");
}

void loop(void) {
  poll_serial();
  delay(2);
}
