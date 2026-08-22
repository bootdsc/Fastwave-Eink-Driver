#ifndef _SSD1677_H_
#define _SSD1677_H_

// SSD1677 driver for the GDEQ0426T82 (4.26", 800x480 mono).
// Decoded from docs/SSD1677-datasheet.pdf and cross-checked against the proven
// GxEPD2_426_GDEQ0426T82 sequence.
//
// ---------------------------------------------------------------------------
// How this part differs from the SSD1683 on the CrowPanel 5.79"
// ---------------------------------------------------------------------------
//   * ONE controller, not a cascade. No 0x80-OR'd slave commands, no seam, no
//     writing every LUT twice. Most of the 5.79's difficulty came from that.
//   * LUT is 105 bytes via 0x32 (the 5.79 wanted 233), holding VS / TP / RP and
//     the frame rate. Voltages go separately via 0x03 / 0x04 / 0x2C.
//   * 10 groups x 4 phases = 40 phases, TP 0..255 frames each (the 5.79 capped
//     at 6 groups and 63 frames), RP 1..256. Considerably more headroom.
//   * X and Y RAM windows are in PIXELS, two bytes each — NOT byte columns.
//   * The gates are physically reversed and the controller has no reverse-scan
//     bit, so Y is programmed decreasing and rows go out 0..H-1 regardless.
//
// ---------------------------------------------------------------------------
// 0x22 activate byte — same bitfield as the SSD1683, ONE meaning changed
// ---------------------------------------------------------------------------
//   b7 enable clock   b6 enable analog   b5 load temperature   b4 load LUT
//   b3 DISPLAY MODE (0 = Mode 1 full, 1 = Mode 2 differential/partial)
//   b2 DISPLAY       b1 disable analog   b0 disable clock
//
// On the SSD1683 b3 selected 3-color vs black/white; here it selects Mode 1 vs
// Mode 2. Do not carry the old interpretation across.
//
//   0xC0 enable clock + analog                        (rails up, no display)
//   0x03 disable analog + clock                       (rails down)
//   0xD7 LUT + Mode 1 + display + power down          (vendor fast full)
//        NB: its temperature bit is CLEAR — the fake 90 C goes in separately
//        via register 0x1A beforehand.
//   0xFC temp + LUT + Mode 2 + display, RAILS LEFT UP (vendor partial)
//   0x0C Mode 2 + display only          <- the fast path: no ramp, no fixed cost
//
// Careful with any byte that has b4 set (0xFC, 0xD7, 0x91): it RELOADS the
// factory LUT and throws away a custom one. On the 5.79 that cost a full
// debugging session because the symptom is simply "the waveform is the vendor's
// again". Re-arm after every clear, seed or init.
//
// The 0x0C trick is what took the 5.79 from 336 ms to 89 ms: the vendor bytes
// ramp the analog block up and down on every single update, and that ramp is a
// fixed cost no waveform change can touch. Bring the rails up once with 0xC0
// and fire bare 0x0C displays instead.

#include "board.h"
#include "spi.h"

// --- low level ---------------------------------------------------------------
void EPD_Reset(void);
uint32_t EPD_WaitBusy_us(uint32_t timeout_ms = 8000);
bool EPD_BusyTimedOut(void);

// --- init --------------------------------------------------------------------
// load_seq picks which OTP waveform is loaded and at what claimed temperature.
// 0xD7 is the vendor "fast" full-update sequence with a fake 90 C temperature
// (register 0x1A = 0x5A), which selects the shortest factory waveform.
void EPD_Init(void);
void EPD_InitFast(void);

// --- RAM addressing ----------------------------------------------------------
// Full-panel window. x/y/w/h are in PIXELS.
void EPD_SetRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void EPD_SetRamFull(void);

// --- image transfer ----------------------------------------------------------
// Buffer is EPD_WBYTES x EPD_H, row-major, MSB = leftmost pixel, 1 = white.
// Windowed write. `fb` is packed w/8 bytes per row by h rows. x and w must be
// multiples of 8. Saves transfer time only — the update still scans every gate
// line, so the waveform costs the same as a full-panel one.
void EPD_WriteRegion(uint8_t reg, const uint8_t *fb, uint16_t x, uint16_t y,
                     uint16_t w, uint16_t h);

void EPD_WriteNew(const uint8_t *fb);              // 0x24
void EPD_WriteOld(const uint8_t *fb, bool invert);  // 0x26, optionally inverted
void EPD_WriteOldMasked(const uint8_t *fb, const uint8_t *mask);  // 0x26, fb^mask
void EPD_FillNew(uint8_t value);

// --- update ------------------------------------------------------------------
// Number of gate lines scanned, via 0x01. Frame period is one scan of all of
// them, so this shortens the waveform itself, not just the RAM transfer.
void EPD_SetGateCount(uint16_t lines);

void EPD_AnalogPower(bool on);   // 0xC0 / 0x03
bool EPD_AnalogIsOn(void);
uint32_t EPD_Update(uint8_t seq22, uint32_t *busy_us_out);  // 0x22 seq, then 0x20

// Convenience wrappers around the documented sequences.
uint32_t EPD_UpdateFullFast(uint32_t *busy_us_out);  // 0xD7
uint32_t EPD_UpdatePartial(uint32_t *busy_us_out);   // 0xFC

// --- RAM content options (0x21) ----------------------------------------------
// "Red RAM" is the 0x26 plane; on a mono panel that is the OLD/previous image.
// Options are Normal / Bypass-as-0 / Inverse, for the OLD plane (high nibble)
// and the NEW plane (low nibble).
enum : uint8_t {
  RAM_NORMAL = 0x00,
  RAM_BYPASS0 = 0x04,
  RAM_INVERSE = 0x08,
};
// old_opt and new_opt take the values above; they are placed in the right nibbles.
void EPD_SetRamOption(uint8_t old_opt, uint8_t new_opt);

// --- waveform (LUT) ----------------------------------------------------------
#define EPD_LUT_BYTES 105
// Load a 105-byte waveform into the LUT register. Voltages are separate.
void EPD_WriteLut(const uint8_t *lut105);
// VGH (0x03), VSH1/VSH2/VSL (0x04), VCOM (0x2C).
void EPD_WriteVoltages(uint8_t vgh, uint8_t vsh1, uint8_t vsh2, uint8_t vsl,
                       uint8_t vcom);

#endif
