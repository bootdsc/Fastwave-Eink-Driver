#ifndef _WAVEFORM_H_
#define _WAVEFORM_H_

// SSD1683 waveform (LUT) construction — decoded from the Solomon Systech
// SSD1683 datasheet Rev 1.0, sections 6.6 / 6.7 (Figure 6-7, b/w mode).
//
// ---------------------------------------------------------------------------
// 233-byte waveform setting
// ---------------------------------------------------------------------------
//   bytes 0..226  -> register 0x32   (LUT; 0xB2 for the cascade slave)
//   byte  227     -> register 0x3F   EOPT   (display-off sequence)
//   byte  228     -> register 0x03   VGH
//   bytes 229..231-> register 0x04   VSH1, VSH2, VSL
//   byte  232     -> register 0x2C   VCOM
//
// In black/white mode there are 5 LUTs, each with 6 groups:
//   block 0 = LUTC (VCOM)   1 = LUTWW   2 = LUTBW   3 = LUTWB   4 = LUTBB
//   block base  = block * 42
//   group base  = block * 42 + group * 7
//
// Each group is 7 bytes, in this order (verified against the Good Display
// GDEY0579T93 partial table, which is only self-consistent this way):
//   [0] RP[n]      group repeat count      (0 = group skipped)
//   [1] phase A    VS[7:6] | TP[5:0]
//   [2] phase B    VS[7:6] | TP[5:0]
//   [3] phase C    VS[7:6] | TP[5:0]
//   [4] phase D    VS[7:6] | TP[5:0]
//   [5] SR[nAB]    repeat count for phases A+B  (0 = sub-group skipped)
//   [6] SR[nCD]    repeat count for phases C+D
//
// TP = phase length in FRAMES, 0..63 (0 skips the phase).
// VS levels (Table 6-6):  00 = VSS / DCVCOM, 01 = VSH1, 10 = VSL, 11 = VSH2.
//
//   byte 224 = FR[3:0] frame rate, byte 225/226 = XON.
//
// FR codes (Table 6-7) — anything else is NOT A VALID SETTING:
//   0x1=25Hz 0x2=50Hz 0x3=75Hz 0x4=100Hz 0x5=125Hz
//   0x9=37.5 0xA=62.5 0xB=87.5 0xC=112.5
// The Good Display table ships FR=0x08, which is undefined on this part — that
// alone may explain why the GD port ran slower and looked wrong here.
//
// Total frames for one group = RP * ( SR_AB*(TP_A+TP_B) + SR_CD*(TP_C+TP_D) ).
// BUSY time is that frame count times the frame period, plus fixed overhead —
// which is exactly why BUSY on this panel is content-independent.
//
// ---------------------------------------------------------------------------
// Voltage encodings (datasheet command table, 0x03 / 0x04 / 0x2C)
// ---------------------------------------------------------------------------
//   VGH  (0x03): 0x17 = 20V, 0x0D = 15V ... step 0.5V from 0x03 = 10V
//   VSH1 (0x04 A): 0x21 = 8.6V ... 0x41 = 15V (POR) ... 0x4B = 17V, step 0.2V
//   VSH2 (0x04 B): 0xA8 = 5V (POR, low range 0x8E..0xCC = 2.4..8.6V)
//   VSL  (0x04 C): 0x0A = -5V ... 0x32 = -15V (POR) ... 0x3A = -17V, step 0.5V
//   EOPT (0x3F): 0x22 = 2 discharge scan frames, 0x07 = keep pixel voltage
//
// Nothing here ever writes OTP (0x30). Everything is volatile register state
// and is undone by any 0x22 sequence carrying a "Load LUT" bit.

#include <Arduino.h>

#define WAVE_BYTES 233

// Voltage-source level codes for the VS field.
enum : uint8_t { VS_VSS = 0, VS_VSH1 = 1, VS_VSL = 2, VS_VSH2 = 3 };

// LUT block indices in the 233-byte table.
enum : uint8_t {
  WB_LUTC = 0,
  WB_LUTWW = 1,
  WB_LUTBW = 2,  // pixel ends WHITE
  WB_LUTWB = 3,  // pixel ends BLACK
  WB_LUTBB = 4,
};

// Parametric three-phase waveform, applied only to the two *changing* LUTs.
// Unchanged pixels (WW / BB) are left at zero, i.e. never driven, which is what
// keeps a short waveform from smearing static text.
//
//   phase A = reset  (opposite polarity kick, helps the pigment let go)
//   phase B = drive  (target polarity — this is what sets contrast)
//   phase C = settle (VSS hold, lets the stack relax before the panel powers down)
// Which of the five voltage registers get pushed with the LUT. These must be
// individually selectable: writing them as a block drags EOPT along, and EOPT
// 0x22 adds two discharge scan frames — a measured +17 ms and a visible flash on
// every update. Any experiment on VCOM alone is worthless if EOPT rides with it.
enum : uint8_t {
  WV_V_NONE = 0,
  WV_V_EOPT = 1,  // 0x3F
  WV_V_VGH = 2,   // 0x03
  WV_V_SRC = 4,   // 0x04 (VSH1/VSH2/VSL)
  WV_V_VCOM = 8,  // 0x2C
  WV_V_ALL = 15,
};

struct WaveParams {
  uint8_t fr;         // FR code, see table above
  uint8_t reset_tp;   // frames, 0..63
  uint8_t drive_tp;   // frames, 1..63
  uint8_t settle_tp;  // frames, 0..63
  uint8_t rp;         // group repeat, 1..255
  uint8_t sr_ab;      // A+B state repeat, 1..255
  bool invert;        // swap which rail drives to black
  bool touch_bb;      // 1-frame VSH1 touch-up on already-black pixels (GD does this)
  // Give the unchanged-pixel LUTs (WW/BB) an equal +VSH1 / -VSL pair instead of
  // leaving them empty. A skipped phase parks the source at VSS against a
  // non-zero VCOM, which is a one-sided DC field on every pixel that is NOT
  // being updated — it accumulates as a creeping darkening of the whole panel.
  // A balanced pair nets to zero charge. It runs alongside the driven blocks on
  // the same timeline, so it costs ZERO extra frames.
  bool balance_idle;
  uint8_t volt_mask;  // WV_V_* bits
  uint8_t vsh1, vsh2, vsl, vcom, vgh, eopt;
};

void wave_defaults(WaveParams *p);

// Build the parametric waveform into a 233-byte buffer.
void wave_build(uint8_t *ws233, const WaveParams *p);

// All five LUTs driven at VS = VSS for `tp` frames: burns exactly the same
// number of frames as a real waveform but applies no field, so BUSY can be
// measured without touching the glass. This is the timing-law probe.
void wave_build_null(uint8_t *ws233, uint8_t tp, uint8_t fr);

// Drive exactly one LUT block, so the glass reveals which transition class it
// actually owns (the BW/WB naming is not trustworthy across vendors).
void wave_build_probe(uint8_t *ws233, uint8_t block, uint8_t level, uint8_t tp,
                      uint8_t fr, const WaveParams *volts);

// Good Display GDEY0579T93 partial table, 26 frames. `fr` overrides byte 224.
void wave_build_gd(uint8_t *ws233, uint8_t fr);

// Frames the waveform will run (max over the driven blocks).
uint16_t wave_frames(const uint8_t *ws233);

// Push a table to the panel. dual = also write the cascade slave (0xB2 etc).
// volt_mask selects which of the five voltage registers ride along.
// Enables the clock first; does not fire an update.
void wave_load(const uint8_t *ws233, bool dual, uint8_t volt_mask);

// FR code -> nominal frame period in microseconds (datasheet Table 6-7).
uint32_t wave_fr_period_us(uint8_t fr);
const char *wave_fr_name(uint8_t fr);
bool wave_fr_valid(uint8_t fr);

void wave_dump(const uint8_t *ws233);

#endif
