#ifndef _EPD_INIT_H_
#define _EPD_INIT_H_

#include "spi.h"

// Dual SSD1683: logical 800x272 (792 visible + 8 seam)
#define EPD_W 800
#define EPD_H 272
#define WHITE 0xFF
#define BLACK 0x00
#define Source_BYTES 50   // 400/8 per chip
#define Gate_BITS 272
#define ALLSCREEN_BYTES (Source_BYTES * Gate_BITS)
#define FB_WBYTES 100     // 800/8 byte columns across the panel

// FRAMEBUFFER LAYOUT: COLUMN-MAJOR, the panel's own order.
//
// The controller wants every row of byte-column 0, then every row of column 1,
// and so on. Storing the buffer row-major meant every byte of a transfer was a
// stride-100 read, which cost ~110 CPU cycles per byte and dominated the 22 ms
// the 20 MHz wire actually needs. Column-major makes each chip's half one
// CONTIGUOUS run, so the transfer is a straight memcpy into the DMA buffer.
//
// Index with FB_IDX. Never assume row-major arithmetic anywhere.
#define FB_IDX(r, c) ((uint32_t)(c) * (uint32_t)Gate_BITS + (uint32_t)(r))

void EPD_GPIOInit(void);  // in spi.cpp

// Timed wait; returns busy time in microseconds (0 if already idle).
// Caps at timeout_ms (default 8000) so a stuck BUSY cannot hang the MCU forever.
uint32_t EPD_WaitBusy_us(uint32_t timeout_ms = 8000);
bool EPD_BusyTimedOut(void);  // last wait hit the cap

// --- Analog rail state ------------------------------------------------------
// Holding the rails up between updates is what removes the ~91-124 ms fixed cost
// per frame, so the bare 0x0C DISPLAY byte measures 0 ms of overhead. That only
// works if the driver's idea of the rail state matches the hardware.
//
// The flag lives here rather than in the app because ONLY the driver sees every
// hardware reset. An app-owned flag goes stale the moment anything pulses RST —
// and then every 0x0C silently does nothing, with busy_us = 0 the sole symptom.
bool EPD_AnalogIsOn(void);
// Power the rails up/down by hand (0x22 = 0xC0 / 0x03). Do this OUTSIDE any
// timed region — the ramp is the fixed cost the fast path exists to avoid.
void EPD_AnalogPower(bool on);
// Update the cached flag for an activate byte written directly by the caller:
// b6 set means the sequence powered the rails itself, b1/b0 that it dropped them.
void EPD_NoteSeqRails(uint8_t seq22);
// Raise the rails if seq22 has b6 clear and they are down. Returns true if it
// acted. Idempotent, so apps that manage rails themselves pay nothing.
bool EPD_EnsureRailsFor(uint8_t seq22);

// Restore full-chip RAM windows after a partial window write (required!)
void EPD_RestoreFullWindows(void);

// Hardware reset with configurable pulse gaps (default vendor-ish 10/10/10)
void EPD_HW_RESET_ms(uint16_t lo_ms, uint16_t mid_ms, uint16_t hi_ms);
void EPD_HW_RESET(void);  // 10,10,10

void EPD_Update(void);
void EPD_PartUpdate(void);
void EPD_FastUpdate(void);
void EPD_DeepSleep(void);
void EPD_Init(void);
void EPD_FastMode1Init(void);  // vendor default: 0xB1 (3-color OTP LUT)
// Same init, explicit OTP LUT select: 0xB1/0x91 = 3-color, 0xB9/0x99 = b/w.
void EPD_FastMode1Init_Ex(uint8_t load_seq);

void EPD_SetRAMMP(void);
void EPD_SetRAMMA(void);
void EPD_SetRAMSP(void);
void EPD_SetRAMSA(void);
void EPD_Display_Clear(void);
void EPD_Display(const uint8_t *ImageBW);
// Write ImageBW into OLD plane (0x26 / 0xA6) so PartUpdate can skip unchanged pixels
void EPD_Display_OldPlane(const uint8_t *ImageBW);
// Same, but invert=true writes ~ImageBW into OLD. Every pixel then differs from NEW,
// so a differential update (0xDC/0xC7) repaints the whole panel at full quality with
// no GC flash. This is bb_epaper's PLANE_FALSE_DIFF. Touches RAM planes only —
// no LUT write, no voltage change, no OTP.
void EPD_Display_OldPlane_Ex(const uint8_t *ImageBW, bool invert);
// OLD = ImageBW ^ mask — per-pixel choice of "drive this again" vs "leave it".
// Set bits keep a half-finished transition being driven on later frames, which
// is what actually clears a trail. Costs no waveform time, only the OLD write.
void EPD_Display_OldPlane_Masked(const uint8_t *ImageBW, const uint8_t *mask);
// White both planes (new+old) — base for trail/dirty partials without full-white flash each step
void EPD_LoadWhiteBothPlanes(void);

// Timed helpers — return SPI-ish wall time (us) excluding prior busy
uint32_t EPD_Display_Timed(const uint8_t *ImageBW, uint32_t *spi_us_out);
uint32_t EPD_PartUpdate_Timed(uint32_t *busy_us_out);
uint32_t EPD_FastUpdate_Timed(uint32_t *busy_us_out);
uint32_t EPD_Update_Timed(uint32_t *busy_us_out);

// --- Custom LUT (volatile register only; never OTP burn 0x30) ---
// SSD1683: Write LUT register 0x32 = 227 bytes WS[0..226], then optional
// 0x3F/0x03/0x04/0x2C from trailing voltage bytes (Waveshare 5.79 style).
#define EPD_LUT_WS_BYTES 227

// Load 227-byte waveform into LUT register (cascade shared SPI / master).
void EPD_WriteLut(const uint8_t *ws227);
// Load full 233-byte table: [0..226]=WS, [227]=EOPT, [228]=VGH, [229..231]=VSH/VSL, [232]=VCOM
void EPD_WriteLutFull(const uint8_t *ws233);
// Same as WriteLut but command address OR'd with tgt (0x00 master, 0x80 slave).
void EPD_WriteLutTarget(uint8_t tgt, const uint8_t *ws227);
// Write voltage trailer [227..232] to tgt (0x3F/0x03/0x04/0x2C).
void EPD_WriteLutVoltagesTarget(uint8_t tgt, const uint8_t *ws233);
// Same, but only the selected registers: 1=EOPT 2=VGH 4=source 8=VCOM.
void EPD_WriteLutVoltagesMaskTarget(uint8_t tgt, const uint8_t *ws233, uint8_t mask);
// Display option 0x37 (Good Display partial path; 10 data bytes).
void EPD_WriteDisplayOpt37Target(uint8_t tgt);
// Reload factory OTP waveform only (~15 ms) — does not touch RAM planes.
void EPD_ReloadOtpLut(void);           // 0x91 (3-color LUT)
void EPD_ReloadOtpLutSeq(uint8_t seq);  // 0x91/0x99/0xB1/0xB9

// Display using *register* LUT — 0xCF = BW display WITHOUT reloading OTP.
// Use this after EPD_WriteLut*; 0xF7 includes Load LUT from OTP.
void EPD_UpdateCustom(void);
uint32_t EPD_UpdateCustom_Timed(uint32_t *busy_us_out);
// Same but 0x22 byte selectable (experiment: 0xCF vs 0xDC after 0x32 write)
uint32_t EPD_UpdateSeq_Timed(uint8_t seq22, uint32_t *busy_us_out);
// Selectable 0x22 byte with NO 0x21 write — isolates the sequence byte alone.
uint32_t EPD_UpdateSeqPlain_Timed(uint8_t seq22, uint32_t *busy_us_out);

// Vendor OTP partial (0xDC) — baseline ~336 ms on this panel
void EPD_PartUpdate_OTP(void);
uint32_t EPD_PartUpdate_OTP_Timed(uint32_t *busy_us_out);

// Windowed write into chip RAM only (no waveform).
// Buffer is full 800x272 FB_WBYTES-stride; window in *byte columns* c0..c1 (0..99)
// and rows r0..r1 (0..271). Splits master (c<50) / slave (c>=50) automatically.
// Writes 0x24 (BW) on each chip; optionally also 0x26/0xA6 with same data for partial.
// Returns SPI wall time us; *bytes_out = number of data bytes clocked.
uint32_t EPD_WriteWindow(const uint8_t *ImageBW, uint8_t c0, uint8_t c1,
                         uint16_t r0, uint16_t r1, bool also_old_ram,
                         uint32_t *bytes_out);
// Same, but OLD gets (ImageBW ^ mask) so trail chasing survives a windowed
// write. mask may be NULL, in which case OLD gets ImageBW unchanged.
// Partial-window coordinate mapping. Default both true (mirrored), matching the
// vendor full-screen register setup. Switchable because the glass disagreed.
bool EPD_WinYMirror(void);
bool EPD_WinXMirror(void);
void EPD_SetWinMirror(bool ymir, bool xmir);
// true = program window ranges as (min,max) bounds; false = (start,end) in scan
// order. Full-panel ranges look identical either way, which is why the vendor's
// sequence cannot tell you which the part actually wants.
bool EPD_WinBounds(void);
void EPD_SetWinBounds(bool b);

uint32_t EPD_WriteWindowMasked(const uint8_t *ImageBW, const uint8_t *mask,
                               uint8_t c0, uint8_t c1, uint16_t r0, uint16_t r1,
                               bool also_old_ram, uint32_t *bytes_out);

#endif
