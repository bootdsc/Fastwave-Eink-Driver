#include "wave1681.h"

#include <string.h>

#define WV_VS_BASE 0     // 5 blocks x 12 group bytes
#define WV_TP_BASE 60    // 12 groups x 7 bytes
#define WV_FR_BASE 144   // 6 bytes, 4 bits per group
#define WV_XON_BASE 150  // 3 bytes, 2 bits per group
#define WV_GROUPS 12

// Byte offsets within a 7-byte group, per order.
struct GroupMap {
  uint8_t tpa, tpb, tpc, tpd, srab, srcd, rp;
};

static GroupMap group_map(uint8_t order) {
  GroupMap m;
  if (order == WAVE1681_ORDER_RP_FIRST) {
    m.rp = 0;
    m.tpa = 1;
    m.tpb = 2;
    m.tpc = 3;
    m.tpd = 4;
    m.srab = 5;
    m.srcd = 6;
  } else {
    m.tpa = 0;
    m.tpb = 1;
    m.srab = 2;
    m.tpc = 3;
    m.tpd = 4;
    m.srcd = 5;
    m.rp = 6;
  }
  return m;
}

void wave1681_build_null(uint8_t *out153, uint8_t tp, uint8_t fr,
                         uint8_t order) {
  memset(out153, 0, WAVE1681_BYTES);

  // All 60 VS bytes stay zero. Every phase of every block parks the source at
  // VSS, so the waveform costs its full time and applies no field at all —
  // the image cannot change, whatever the rest of this table says.
  const GroupMap m = group_map(order);
  uint8_t *g = out153 + WV_TP_BASE;  // group 0; groups 1..11 stay zeroed
  // Repeats stay 0, which is one pass. Only TPA carries frames, so
  // frames = tp and the slope is the frame period with no intercept.
  g[m.tpa] = tp;

  // Same nibble in both halves of every FR byte, so the group-to-nibble
  // packing does not have to be known in order to sweep the rate.
  const uint8_t frb = (uint8_t)((fr & 0x0F) | ((fr & 0x0F) << 4));
  for (uint8_t i = 0; i < 6; i++) out153[WV_FR_BASE + i] = frb;

  // XON left at 0.
  (void)WV_XON_BASE;
}

static inline uint8_t vs_pack(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  return (uint8_t)((a << 6) | (b << 4) | (c << 2) | d);
}

void wave1681_defaults(Wave1681 *w) {
  // fr 4 (100 Hz, 10.08 ms) over fr 5 (125 Hz, 8.06 ms): the 25% longer pulse
  // switches the ink visibly harder for the same total time, and at drive 5 it
  // is FASTER than the old drive-8-at-fr-5 default as well. Picked on glass.
  w->fr = WV_FR_100;
  w->reset_tp = 0;
  // Measured contrast against drive frames, SINGLE-SHOT white->card on this
  // glass: 1 -> 13 (below threshold, nothing moves), 2 -> 114, 3..6 -> 116,
  // 8+ -> 121 and flat. So the single-transition knee is 2 frames.
  //
  // 5 frames at fr 4 = 50.4 ms of drive, 6 frames total = 60.5 ms busy,
  // ~15 fps measured. That beat drive 8 at fr 5 (72.6 ms, 13.0 fps) on BOTH
  // darkness and speed once fr was tried, which is why the fr/drive pair is
  // chosen together rather than drive alone.
  //
  // Contrast against drive frames at fr 5, for reference: streaming a 50%-ink
  // sweeping bar gave 93 / 118 / 127 / 132 at drive 2 / 4 / 8 / 16, against
  // 114 / 116 / 121 / 121 single-shot -- so continuous video costs real
  // contrast at the fast end and almost nothing past drive 8.
  w->drive_tp = 5;
  w->settle_tp = 1;
  w->rp = 0;         // 0 = run once
  w->invert = false;
  w->blk_black = 3;  // LUTWB, ends black
  w->blk_white = 2;  // LUTBW, ends white
  // 10 on glass. The top-up only fires while a pixel is fresh, so depth buys
  // darkness without the DC that driving the BB class from the LUT would.
  w->bb_tp = 10;
  w->ww_tp = 0;      // off
  w->blk_bb = 4;     // LUTBB
  // POR rails. VSH1 0x4B / VSL 0x3A is ~13% more impulse at an identical frame
  // count, but panel voltages do not move without being asked for.
  w->vsh1 = 0x41;  // +15 V
  w->vsh2 = 0xA8;  // +5 V, low range
  w->vsl = 0x32;   // -15 V
  // -0.2 V. On the 5.79" a short waveform made the whole panel creep darker
  // every update, because pixels with a skipped phase sit at VSS against a
  // factory VCOM bias well below zero — a small DC field on every update, and
  // it accumulates. This one register cured it for zero frames and zero ms.
  // 0x00 is NOT a defined value; the register starts at 0x08.
  w->vcom = 0x08;
}

void wave1681_build(uint8_t *out153, const Wave1681 *w) {
  memset(out153, 0, WAVE1681_BYTES);

  const uint8_t to_black = w->invert ? WV_VSH1 : WV_VSL;
  const uint8_t to_white = w->invert ? WV_VSL : WV_VSH1;
  const uint8_t bb = w->blk_black < 5 ? w->blk_black : 3;
  const uint8_t bw = w->blk_white < 5 ? w->blk_white : 2;

  // Group 0 only. Phase A reset, B drive, C settle, D unused. A phase with
  // TP = 0 is skipped whatever its VS says, so a zero reset costs nothing.
  //
  // The BB class is deliberately NOT given a level here. Driving it from the
  // LUT hits every black pixel on every frame, which is DC on static content
  // and darkens the whole field. The black top-up is done by re-classifying
  // selected pixels through the OLD plane instead -- see bb_mask_build() in
  // inkvideo_154.cpp -- so it fires only while a pixel is fresh.
  out153[(uint32_t)bb * 12 + 0] =
      vs_pack(w->reset_tp ? to_white : WV_VSS, to_black, WV_VSS, WV_VSS);
  out153[(uint32_t)bw * 12 + 0] =
      vs_pack(w->reset_tp ? to_black : WV_VSS, to_white, WV_VSS, WV_VSS);

  // [TPA, TPB, SRAB, TPC, TPD, SRCD, RP], repeats are n+1 (0 = once).
  uint8_t *g = out153 + WV_TP_BASE;
  g[0] = w->reset_tp;
  g[1] = w->drive_tp;
  g[2] = 0;             // SRAB -> 1
  g[3] = w->settle_tp;  // TPC
  g[4] = 0;             // TPD
  g[5] = 0;             // SRCD -> 1
  g[6] = w->rp;         // RP -> rp+1

  const uint8_t frb = (uint8_t)((w->fr & 0x0F) | ((w->fr & 0x0F) << 4));
  for (uint8_t i = 0; i < 6; i++) out153[WV_FR_BASE + i] = frb;
}

uint16_t wave1681_frames(const uint8_t *out153, uint8_t order) {
  const GroupMap m = group_map(order);
  uint32_t total = 0;
  for (uint8_t i = 0; i < WV_GROUPS; i++) {
    const uint8_t *g = out153 + WV_TP_BASE + (uint32_t)i * 7;
      // Repeat fields are n+1. This is the whole law.
    const uint32_t f =
        ((uint32_t)g[m.rp] + 1u) *
        (((uint32_t)g[m.srab] + 1u) * ((uint32_t)g[m.tpa] + g[m.tpb]) +
         ((uint32_t)g[m.srcd] + 1u) * ((uint32_t)g[m.tpc] + g[m.tpd]));
    total += f;
  }
  if (total > 65535u) total = 65535u;
  return (uint16_t)total;
}

void wave1681_dump(const uint8_t *out153, uint8_t order) {
  const GroupMap m = group_map(order);
  Serial.printf("LUT153 order=%s frames=%u fr=0x%02X\n",
                order ? "RP-first" : "TP-first",
                (unsigned)wave1681_frames(out153, order),
                (unsigned)(out153[WV_FR_BASE] & 0x0F));
  for (uint8_t i = 0; i < WV_GROUPS; i++) {
    const uint8_t *g = out153 + WV_TP_BASE + (uint32_t)i * 7;
    if (!g[m.rp]) continue;
    Serial.printf("  g%-2u rp=%-3u tp=%u/%u/%u/%u sr=%u/%u  vs=%02X%02X%02X%02X%02X\n",
                  (unsigned)i, (unsigned)g[m.rp], (unsigned)g[m.tpa],
                  (unsigned)g[m.tpb], (unsigned)g[m.tpc], (unsigned)g[m.tpd],
                  (unsigned)g[m.srab], (unsigned)g[m.srcd],
                  (unsigned)out153[WV_VS_BASE + 0 * 12 + i],
                  (unsigned)out153[WV_VS_BASE + 1 * 12 + i],
                  (unsigned)out153[WV_VS_BASE + 2 * 12 + i],
                  (unsigned)out153[WV_VS_BASE + 3 * 12 + i],
                  (unsigned)out153[WV_VS_BASE + 4 * 12 + i]);
  }
}
