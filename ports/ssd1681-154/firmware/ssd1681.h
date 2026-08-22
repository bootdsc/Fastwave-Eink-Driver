#ifndef _SSD1681_H_
#define _SSD1681_H_

// SSD1681 for GDEH0154D67 200x200. Own init, not GxEPD "fast partial".
// OTP waveform until a custom LUT is measured on this glass.
// 0x22 b3 is Mode 1 vs Mode 2 on this family (not SSD1683 3-colour).

#include "board.h"
#include "spi.h"

void EPD_Reset(void);
uint32_t EPD_WaitBusy_us(uint32_t timeout_ms = 8000);
bool EPD_BusyTimedOut(void);

void EPD_Init(void);
void EPD_SetRamFull(void);
void EPD_WriteNew(const uint8_t *fb);
void EPD_WriteOld(const uint8_t *fb, bool invert);
void EPD_WriteOldMasked(const uint8_t *fb, const uint8_t *mask);

void EPD_AnalogPower(bool on);
bool EPD_AnalogIsOn(void);
void EPD_UpdateKick(uint8_t seq22);
uint32_t EPD_Update(uint8_t seq22, uint32_t *busy_us_out);

enum : uint8_t {
  RAM_NORMAL = 0x00,
  RAM_BYPASS0 = 0x04,
  RAM_INVERSE = 0x08,
};
void EPD_SetRamOption(uint8_t old_opt, uint8_t new_opt);

// 153 bytes to 0x32. A custom LUT lives in RAM and is DISCARDED by any 0x22
// activate byte carrying b4 (load LUT from OTP) — 0x1C and 0xFC both do. Run a
// custom waveform with 0x0C (display only, rails already up) or 0x04.
void EPD_WriteLut(const uint8_t *lut153);

// 0x03 VGH, 0x04 VSH1/VSH2/VSL, 0x2C VCOM. Written individually and only where
// asked: on the SSD1683 the block trailer carried EOPT 0x22, which costs +17 ms
// and a visible flash on every update because it is two extra discharge scan
// frames. Pass 0 for any rail to leave it alone.
void EPD_WriteVoltages(uint8_t vgh, uint8_t vsh1, uint8_t vsh2, uint8_t vsl,
                       uint8_t vcom);

// 0x3F EOPT, the display-off sequence. POR is 0x22 = two discharge SCAN
// FRAMES, which cost real time on every single update; 0x07 = keep pixel
// voltage, i.e. no discharge scan. On the SSD1683 this register alone was
// +17 ms per update. Never write it as part of a register block -- that is how
// it gets set by accident.
void EPD_WriteEopt(uint8_t eopt);

// 0x3A dummy line period, 0x3B gate line width — the frame period below what
// the LUT's FR field can reach. Nothing in this tree has ever written either;
// they sit at POR. Pass 0 to leave one alone.
void EPD_WriteLineTiming(uint8_t dummy_line, uint8_t gate_width);

#endif
