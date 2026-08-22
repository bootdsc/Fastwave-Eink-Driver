#ifndef _WAVE1677_H_
#define _WAVE1677_H_

// SSD1677 waveform construction, 105 bytes for register 0x32.
//
// Layout (datasheet section 6.6/6.7, confirmed against the Xteink X4 community
// firmware which carries the OEM tables verbatim):
//
//   bytes   0..49   VS levels — 5 LUTs x 10 bytes
//                   LUT0..LUT3 = source levels for the four transition classes,
//                   LUT4 = VCOM. Each byte packs 4 phases at 2 bits per phase,
//                   so 10 bytes covers Group0..Group9 phases A..D.
//   bytes  50..99   TP/RP — 10 groups x 5 bytes: TPnA, TPnB, TPnC, TPnD, RPn
//   bytes 100..104  frame rate, 5 bytes
//
// Voltages are NOT in this block; they go via 0x03 / 0x04 / 0x2C.
//
// Compared with the SSD1683 on the 5.79": 10 groups instead of 6, four phases
// each, and TP counts up to 255 frames instead of 63. Far more room to shape a
// waveform, and the same "BUSY = frames x frame period" law applies.
//
// VS level codes (2 bits per phase), same encoding as the SSD1683:
//   00 = VSS (no drive)   01 = VSH1   10 = VSL   11 = VSH2

#include <Arduino.h>

#define WAVE1677_BYTES 105

enum : uint8_t { VS_VSS = 0, VS_VSH1 = 1, VS_VSL = 2, VS_VSH2 = 3 };

// Which of the four source LUTs a transition uses. Which physical transition
// each index corresponds to is panel convention, not something the datasheet
// pins down — `probe` settles it on glass rather than by assumption.
enum : uint8_t { LUT_0 = 0, LUT_1 = 1, LUT_2 = 2, LUT_3 = 3, LUT_VCOM = 4 };

struct Wave1677 {
  uint8_t fr;         // frame-rate code written to all 5 frame-rate bytes
  uint8_t drive_tp;   // frames of target-polarity drive
  uint8_t settle_tp;  // frames at VSS afterwards (0 = none)
  uint8_t rp;         // group repeat, 1..255
  bool invert;        // swap which rail drives toward black
  uint8_t vgh, vsh1, vsh2, vsl, vcom;
};

void wave1677_defaults(Wave1677 *w);

// Build a minimal one-group waveform: LUT1 drives one way, LUT2 the other.
void wave1677_build(uint8_t *out105, const Wave1677 *w);

// 4-level grayscale.
//
// In grey mode the two RAM planes stop meaning new/old and become BITPLANES:
// 0x24 carries the high bit, 0x26 the low bit, so each pixel has a 2-bit code
// and each code selects one of LUT0..LUT3. Four native levels, one waveform.
//
// Built rather than copied: bb_epaper's 4-grey table for this panel decodes to
// RP = 0 (every group skipped) under the byte order measured here, and a custom
// waveform of 12 drive + 1 settle frames came back as exactly 13 frames, which
// pins the order as TPA,TPB,TPC,TPD,RP. Rather than trust a table that cannot
// be reconciled, this constructs the ramp explicitly:
//
//   Group 0        every code driven to black for `reset_tp` frames — a common
//                  starting state, since grey levels are set by how far back
//                  toward white each one is then pulled
//   Groups 1..3    `step_tp` frames each, driving progressively fewer codes
//                  toward white, so code 3 gets 3 steps, code 2 gets 2, code 1
//                  gets 1 and code 0 stays black
//
// Timing is per group and shared by all four LUTs — only the VOLTAGE differs per
// code — which is why the ramp has to be built from separate groups rather than
// by giving each level its own phase length.
void wave1677_build_gray(uint8_t *out105, uint8_t reset_tp, uint8_t step_tp,
                         uint8_t fr, bool invert);

// Graduated black-ward drive with NO reset group — four levels in one pass and
// no flash, because nothing is driven to a common state first. Levels are
// relative to whatever is already on the glass, so this is only clean starting
// from a known field (e.g. white).
void wave1677_build_levels(uint8_t *out105, uint8_t step_tp, uint8_t fr, bool invert);

// Every LUT at VS = VSS for `tp` frames: burns the full time cost with no field
// across the pixels, so BUSY can be measured without touching the image. This is
// the probe that established the timing law on the 5.79 and it costs nothing to
// run here.
void wave1677_build_null(uint8_t *out105, uint8_t tp, uint8_t fr);

// Total frames the waveform will run (max across the groups).
uint16_t wave1677_frames(const uint8_t *out105);

void wave1677_dump(const uint8_t *out105);

#endif
