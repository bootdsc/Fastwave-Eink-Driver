#ifndef _WAVE1681_H_
#define _WAVE1681_H_

// SSD1681 waveform (LUT) construction, 153 bytes for register 0x32.
//
// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
//   bytes   0..59   VS levels — 5 LUTs x 12 bytes. LUT0..LUT3 are the source
//                   levels for the four transition classes, LUT4 is VCOM. Each
//                   byte is one GROUP and packs its four phases at 2 bits per
//                   phase: [7:6]=A [5:4]=B [3:2]=C [1:0]=D. 12 groups.
//   bytes  60..143  TP/RP/SR — 12 groups x 7 bytes.
//   bytes 144..149  FR frame rate — 4 bits per group, two groups per byte.
//   bytes 150..152  XON — 2 bits per group.
//
// Voltages are NOT in this block; they go via 0x03 / 0x04 / 0x2C.
//
// Compared with the SSD1683 on the 5.79": 12 groups instead of 6, and VS lives
// in its own array so TP gets a whole byte (0..255) instead of sharing one with
// VS. Same timing law:
//
// ---------------------------------------------------------------------------
// The timing law, MEASURED (`calib`, `seqscan`, `bytescan`)
// ---------------------------------------------------------------------------
//   group bytes   [TPA, TPB, SRAB, TPC, TPD, SRCD, RP]
//   frames        (RP+1) * ( (SRAB+1)*(TPA+TPB) + (SRCD+1)*(TPC+TPD) )
//   busy          frames * frame_period          <- no extra frame
//   total frames  max over the five blocks (they share one timeline)
//
// **Every repeat field is n+1: a stored 0 runs the group ONCE.** That single
// fact is what makes the vendor tables decode, and getting it wrong is how a
// waveform comes out 2x to 4x longer than intended while BUSY stays perfectly
// linear — so the error hides.
//
// Settled by perturbing one group byte at a time from a baseline of
// 1,8,0,0,0,1,0 (9 frames). Probe value 4 gave 12 / 5 / 45 / 17 / 17 / 9 / 45
// frames for bytes 0..6, and only the law above reproduces all seven. It then
// retro-predicted two earlier surprises exactly: a table of tp,0,1,0,0,0,1
// running 4*tp frames, and 1,0,5,1,0,1,1 running 16.
//
// `order` is kept as a parameter only so the null probe can still reproduce
// those historical measurements. WAVE1681_ORDER_TP_FIRST is the truth.
//
// A custom LUT lives in RAM and is discarded by any 0x22 activate byte carrying
// b4 (load LUT from OTP) — `0x1C` and `0xFC` both do. Run a custom waveform with
// `0x0C` (display only, rails already up) or `0x04`.

#include <Arduino.h>

#define WAVE1681_BYTES 153

// VS level codes, 2 bits per phase.
enum : uint8_t { WV_VSS = 0, WV_VSH1 = 1, WV_VSL = 2, WV_VSH2 = 3 };

enum : uint8_t {
  WAVE1681_ORDER_TP_FIRST = 0,  // TPA TPB SRAB TPC TPD SRCD RP
  WAVE1681_ORDER_RP_FIRST = 1,  // RP TPA TPB TPC TPD SRAB SRCD
};

// FR codes as measured on the SSD1683. Unverified on this part — that is what
// the sweep is for. Anything outside the table is undefined on the 1683.
// Two runs of codes, and the gap between them is real: 0x06..0x08 are marked
// NA on this family. The 0x09..0x0C run is the half-step rates, which is where
// the useful in-between values live -- 0x0B (87.5 Hz) sits between 0x03 and
// 0x04, and 0x0C (112.5 Hz) between 0x04 and 0x05.
enum : uint8_t {
  WV_FR_25 = 0x01,
  WV_FR_50 = 0x02,
  WV_FR_75 = 0x03,
  WV_FR_100 = 0x04,
  WV_FR_125 = 0x05,
  WV_FR_37 = 0x09,   // 37.5 Hz
  WV_FR_62 = 0x0A,   // 62.5 Hz
  WV_FR_87 = 0x0B,   // 87.5 Hz
  WV_FR_112 = 0x0C,  // 112.5 Hz
};

// True if the code is one the part defines.
static inline bool wv_fr_valid(uint8_t fr) {
  return (fr >= 0x01 && fr <= 0x05) || (fr >= 0x09 && fr <= 0x0C);
}

// A parametric three-phase waveform, applied ONLY to the two changing
// transition classes. The unchanged classes are left at zero, i.e. never
// driven — which is what stops a short waveform from smearing static content.
//
//   phase A = reset   opposite-polarity kick, helps the pigment let go
//   phase B = drive   target polarity. This is what sets contrast.
//   phase C = settle  VSS hold, lets the stack relax before power-down
//
// frames = RP * ( SR_AB*(reset + drive) + SR_CD*(settle + 0) )
//        = RP * (reset + drive + settle)      with both SR at 1
// busy   = (frames + 1) * frame_period
//
// reset MUST be 0 when trail chasing: a re-driven pixel runs the whole LUT
// again, so the opposite-polarity kick replays as a dark flash on pixels that
// were already clean, and a staggered history reads as a pulse travelling
// behind the moving content. Measured on the 5.79".
//
// blk_black / blk_white select which of the five blocks are the two CHANGING
// classes. Block order follows the SSD1683: 0 = LUTC (VCOM), 1 = WW, 2 = BW,
// 3 = WB, 4 = BB. So the changing pair is 3 (ends black) and 2 (ends white),
// and 1 / 4 must stay at zero or static content gets driven.
//
// Driving 1 and 2 by mistake pushed every unchanged white pixel toward black
// and wiped the test card to a uniform grey — which is what identified the
// convention. `map A B` walks it live if a panel disagrees.
struct Wave1681 {
  uint8_t fr;         // frame-rate code, written to all 12 groups
  uint8_t reset_tp;   // opposite-polarity kick frames (0 = no kick)
  uint8_t drive_tp;   // target-polarity drive frames
  uint8_t settle_tp;  // VSS hold frames
  uint8_t rp;         // extra group repeats; 0 = run once
  bool invert;        // swap which rail drives toward black
  uint8_t blk_black, blk_white;
  // Black top-up DEPTH, in frames of change history, 0..20. Not a LUT field:
  // it is applied through the OLD plane by bb_mask_build() in
  // inkvideo_154.cpp, so it costs no waveform time.
  //
  // A pixel that has just gone black is re-classified W->B for the next
  // bb_tp frames, so it takes bb_tp extra full drives and then stops -- it
  // falls out of the history and is never touched again until it changes.
  // That is the whole point: driving the BB class from the LUT instead hits
  // every black pixel every frame, which is DC on static content and darkens
  // the field.
  //
  // White is deliberately never re-driven: it comes back too weak at these
  // frame counts and the field goes dark.
  uint8_t bb_tp;
  uint8_t blk_bb;
  // The mirror of bb_tp for white: extra drives for a pixel that has just gone
  // WHITE, then left alone. Same free mechanism through the OLD plane.
  //
  // Re-driving white indiscriminately (which is what trail/chase does) pulls
  // the field dark, because the to-white drive lands short at these frame
  // counts and a pixel that never completes sits grey. Restricting it to
  // freshly-changed pixels is the same fix bb_tp applies to black.
  //
  // The paired lever is VSH1: with invert=0 the to-white rail is VSH1, POR
  // 0x41 = 15 V, and 0x4B = 17 V is in spec. `volts 4B 32` raises white alone
  // and leaves black at 15 V. Untested -- if ww still greys the field, that is
  // the first thing to try.
  uint8_t ww_tp;
  uint8_t vsh1, vsh2, vsl, vcom;
};

void wave1681_defaults(Wave1681 *w);
void wave1681_build(uint8_t *out153, const Wave1681 *w);

// Every VS phase at VSS: burns the full time cost with ZERO field across the
// pixel, so BUSY can be measured without touching the image. One group runs
// `tp` frames, the other eleven are skipped.
void wave1681_build_null(uint8_t *out153, uint8_t tp, uint8_t fr, uint8_t order);

// Frames the table will run, decoded under `order`. 0 means the order is wrong
// (or the group really is skipped).
uint16_t wave1681_frames(const uint8_t *out153, uint8_t order);

void wave1681_dump(const uint8_t *out153, uint8_t order);

#endif
