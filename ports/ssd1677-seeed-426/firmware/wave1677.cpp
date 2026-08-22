#include "wave1677.h"

#include <string.h>

#define VS_BASE 0    // bytes 0..49  : 5 LUTs x 10
#define TP_BASE 50   // bytes 50..99 : 10 groups x 5
#define FR_BASE 100  // bytes 100..104

// One VS byte holds four phases, 2 bits each, phase A in the high bits.
static inline uint8_t vs_byte(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
  return (uint8_t)(((a & 3) << 6) | ((b & 3) << 4) | ((c & 3) << 2) | (d & 3));
}

// VS byte for group `grp` of LUT `lut`.
static inline uint8_t *vs_at(uint8_t *o, uint8_t lut, uint8_t grp)
{
  return o + VS_BASE + (uint32_t)lut * 10 + grp;
}

// TP/RP record for group `grp`: TPnA, TPnB, TPnC, TPnD, RPn.
static inline uint8_t *tp_at(uint8_t *o, uint8_t grp)
{
  return o + TP_BASE + (uint32_t)grp * 5;
}

void wave1677_defaults(Wave1677 *w)
{
  // 0x88 = the fastest rate `calib` found on this panel: period is 79.3/N ms per
  // nibble, so N=8 gives 9.92 ms/frame. The OEM tables ship 0x22 (39.6 ms), which
  // is four times slower for no benefit we can see.
  w->fr = 0x88;
  w->drive_tp = 12;
  w->settle_tp = 1;
  w->rp = 1;
  // TRUE on this panel: measured on glass 2026-08-08, the default mapping drove
  // the disc white and its trail black — i.e. every transition ran backwards.
  // LUT1/LUT2 are the other way round here than on the SSD1683.
  w->invert = true;
  // Power-on / OEM values. Writing these changes nothing, so a waveform built
  // with them is a true phases-only experiment.
  w->vgh = 0x17;   // 20 V
  w->vsh1 = 0x41;  // 15 V
  w->vsh2 = 0xA8;  // 5 V
  w->vsl = 0x32;   // -15 V
  w->vcom = 0x30;
}

void wave1677_build(uint8_t *out105, const Wave1677 *w)
{
  memset(out105, 0, WAVE1677_BYTES);

  uint8_t to_a = w->invert ? VS_VSL : VS_VSH1;
  uint8_t to_b = w->invert ? VS_VSH1 : VS_VSL;
  uint8_t drive = w->drive_tp ? w->drive_tp : 1;
  uint8_t rp = w->rp ? w->rp : 1;

  // Group 0 only. Phase A drives, phase B settles at VSS, C and D unused.
  // No opposite-polarity kick: on the 5.79 that kick replayed as a dark flash
  // whenever a pixel was re-driven, and repeated drives supply the release it
  // was there to provide.
  *vs_at(out105, LUT_1, 0) = vs_byte(to_a, VS_VSS, VS_VSS, VS_VSS);
  *vs_at(out105, LUT_2, 0) = vs_byte(to_b, VS_VSS, VS_VSS, VS_VSS);
  // LUT0 / LUT3 left at VSS: pixels that are not changing are not driven, which
  // is what stops a short waveform smearing static content.
  // LUT4 (VCOM) left at VSS = DCVCOM throughout.

  uint8_t *g0 = tp_at(out105, 0);
  g0[0] = drive;                          // TP0A
  g0[1] = w->settle_tp;                   // TP0B
  g0[2] = 0;                              // TP0C
  g0[3] = 0;                              // TP0D
  g0[4] = rp;                             // RP0

  memset(out105 + FR_BASE, w->fr, 5);
}

void wave1677_build_gray(uint8_t *out105, uint8_t reset_tp, uint8_t step_tp,
                         uint8_t fr, bool invert)
{
  memset(out105, 0, WAVE1677_BYTES);
  if (!reset_tp) reset_tp = 1;
  if (!step_tp) step_tp = 1;

  const uint8_t to_black = invert ? VS_VSH1 : VS_VSL;
  const uint8_t to_white = invert ? VS_VSL : VS_VSH1;

  // Group 0: drive every code hard to black so all four start together.
  for (uint8_t l = LUT_0; l <= LUT_3; l++)
    *vs_at(out105, l, 0) = vs_byte(to_black, VS_VSS, VS_VSS, VS_VSS);
  uint8_t *g0 = tp_at(out105, 0);
  g0[0] = reset_tp;
  g0[4] = 1;

  // Groups 1..3: pull codes back toward white, one fewer code each group.
  // After group 3: LUT3 has had 3 steps of white, LUT2 two, LUT1 one, LUT0 none.
  for (uint8_t g = 1; g <= 3; g++) {
    for (uint8_t l = LUT_0; l <= LUT_3; l++) {
      bool drive = (l >= (LUT_0 + g));  // g=1 -> L1..L3, g=2 -> L2,L3, g=3 -> L3
      *vs_at(out105, l, g) =
          vs_byte(drive ? to_white : VS_VSS, VS_VSS, VS_VSS, VS_VSS);
    }
    uint8_t *gg = tp_at(out105, g);
    gg[0] = step_tp;
    gg[4] = 1;
  }

  memset(out105 + FR_BASE, fr, 5);
}

void wave1677_build_levels(uint8_t *out105, uint8_t step_tp, uint8_t fr, bool invert)
{
  memset(out105, 0, WAVE1677_BYTES);
  if (!step_tp) step_tp = 1;
  const uint8_t to_black = invert ? VS_VSH1 : VS_VSL;

  // NO reset group. Each code simply gets a different amount of black-ward drive
  // from wherever the pixel already is:
  //   code 0 -> 3 steps (darkest)   code 2 -> 1 step
  //   code 1 -> 2 steps             code 3 -> none, stays as it was
  //
  // Dropping the common reset is what removes the flash — the reset is what
  // drove every pixel to black before pulling it back, and doing that once per
  // frame is a full-panel black/white cycle.
  //
  // The price is that levels are now RELATIVE, not absolute: a pixel that should
  // hold its shade is still driven every frame and will creep darker. That is
  // fine for a one-shot like this and is exactly what a shadow buffer with
  // per-pixel code selection would fix.
  for (uint8_t g = 0; g < 3; g++) {
    for (uint8_t l = LUT_0; l <= LUT_3; l++) {
      bool drive = (l + g) < 3;  // g=0 -> L0,L1,L2; g=1 -> L0,L1; g=2 -> L0
      *vs_at(out105, l, g) =
          vs_byte(drive ? to_black : VS_VSS, VS_VSS, VS_VSS, VS_VSS);
    }
    uint8_t *gg = tp_at(out105, g);
    gg[0] = step_tp;
    gg[4] = 1;
  }

  memset(out105 + FR_BASE, fr, 5);
}

void wave1677_build_null(uint8_t *out105, uint8_t tp, uint8_t fr)
{
  memset(out105, 0, WAVE1677_BYTES);
  if (!tp) tp = 1;
  // Every VS stays 00 (VSS) — the phases still burn their frames but no field
  // appears across any pixel, so the image cannot change.
  uint8_t *g0 = tp_at(out105, 0);
  g0[0] = tp;
  g0[4] = 1;
  memset(out105 + FR_BASE, fr, 5);
}

uint16_t wave1677_frames(const uint8_t *out105)
{
  uint32_t total = 0;
  for (uint8_t g = 0; g < 10; g++) {
    const uint8_t *t = out105 + TP_BASE + (uint32_t)g * 5;
    uint32_t sum = (uint32_t)t[0] + t[1] + t[2] + t[3];
    total += sum * (t[4] ? t[4] : 0);
  }
  return (uint16_t)(total > 0xFFFF ? 0xFFFF : total);
}

void wave1677_dump(const uint8_t *out105)
{
  for (uint8_t g = 0; g < 10; g++) {
    const uint8_t *t = out105 + TP_BASE + (uint32_t)g * 5;
    if (!t[4] || !(t[0] | t[1] | t[2] | t[3])) continue;
    Serial.printf("  G%u  TP %u/%u/%u/%u  RP %u   VS", (unsigned)g, t[0], t[1], t[2],
                  t[3], t[4]);
    for (uint8_t l = 0; l < 5; l++)
      Serial.printf(" L%u=%02X", (unsigned)l, out105[VS_BASE + l * 10 + g]);
    Serial.println();
  }
  Serial.printf("  FR=%02X frames=%u\n", out105[FR_BASE],
                (unsigned)wave1677_frames(out105));
}
