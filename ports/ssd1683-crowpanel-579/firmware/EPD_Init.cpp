#include "EPD_Init.h"

static bool g_busy_timed_out = false;

// Are the analog rails currently energised?
//
// This lives in the DRIVER, not the app, because only the driver sees every
// hardware reset. Apps used to keep their own `rails_up` bool, and nothing
// cleared it when a re-init pulsed RST — so after the busy-timeout recovery path
// (reinit + reseed) the flag still said "up" while the rails were actually down.
// Every subsequent bare 0x0C then skipped powering them and silently did
// NOTHING, with busy_us = 0 the only symptom and frozen glass the only visible
// effect. Backported from the SSD1677 tree, which lost a session to exactly this.
static bool g_analog_on = false;

// Which way a partial window maps buffer coordinates onto chip RAM. Runtime
// toggles so the panel can settle it directly — see write_chip_window().
static bool g_win_ymirror = true;
static bool g_win_xmirror = true;

// How 0x44/0x45 (and 0xC4/0xC5) express a range.
//   false = (start, end) following the scan direction — the vendor's literal form
//   true  = (min, max) bounds, with the address counter alone choosing direction
//
// The vendor only ever programs FULL-PANEL ranges, where both readings select
// the same rows, so its (271, 0) proves nothing. For a band they differ утterly:
// programmed as (239, 32) when the part wants min-first, the range wraps through
// 271 and 0 and the write lands in the rows the band EXCLUDES — which is exactly
// where the bars appear, and why only a full-panel write clears them.
// SETTLED: false. Programming bounds form made the image land at wrong addresses
// entirely (content jumping around the panel), so the part does want
// (start, end) following the scan direction — the vendor's literal form. The
// window range registers were never the cause of the stripes.
static bool g_win_bounds = false;

bool EPD_WinYMirror(void) { return g_win_ymirror; }
bool EPD_WinXMirror(void) { return g_win_xmirror; }
bool EPD_WinBounds(void) { return g_win_bounds; }
void EPD_SetWinMirror(bool ymir, bool xmir) { g_win_ymirror = ymir; g_win_xmirror = xmir; }
void EPD_SetWinBounds(bool b) { g_win_bounds = b; }

bool EPD_BusyTimedOut(void) { return g_busy_timed_out; }

bool EPD_AnalogIsOn(void) { return g_analog_on; }

// Bring the analog rails up (or down) outside any timed path. Every 0x22 byte
// with b6 clear assumes this has already happened.
void EPD_AnalogPower(bool on)
{
  EPD_GPIOInit();
  EPD_WR_REG(0x22);
  EPD_WR_DATA8(on ? 0xC0 : 0x03);
  EPD_WR_REG(0x20);
  (void)EPD_WaitBusy_us(2000);
  g_analog_on = on;
}

// Keep the cached flag honest for an arbitrary activate byte.
//   b6 set  -> the sequence powers the rails itself, so they are up afterwards
//   b1/b0   -> the sequence tears them down again
// Call this from anything that writes 0x22 directly.
void EPD_NoteSeqRails(uint8_t seq22)
{
  if (seq22 & 0x40) g_analog_on = true;
  if (seq22 & 0x03) g_analog_on = false;
}

// Raise the rails if `seq22` needs them already up. Returns true if it had to.
bool EPD_EnsureRailsFor(uint8_t seq22)
{
  if (!(seq22 & 0x40) && !g_analog_on) {
    EPD_AnalogPower(true);
    return true;
  }
  return false;
}

uint32_t EPD_WaitBusy_us(uint32_t timeout_ms)
{
  g_busy_timed_out = false;
  uint32_t t0 = micros();
  uint32_t limit = timeout_ms * 1000UL;
  if (limit < 1000) limit = 1000;
  while (EPD_ReadBUSY != 0) {
    if ((micros() - t0) > limit) {
      g_busy_timed_out = true;
      break;
    }
    // Tight spin for hundreds of ms reboots under the task WDT while scrolling
    yield();
  }
  return micros() - t0;
}

void EPD_RestoreFullWindows(void)
{
  // Leave partial window mode; next full Display/Part must see whole chip
  EPD_SetRAMMP();
  EPD_SetRAMMA();
  EPD_SetRAMSP();
  EPD_SetRAMSA();
}

void EPD_HW_RESET_ms(uint16_t lo_ms, uint16_t mid_ms, uint16_t hi_ms)
{
  if (lo_ms) delay(lo_ms);
  EPD_RES_Clr();
  if (mid_ms) delay(mid_ms);
  EPD_RES_Set();
  if (hi_ms) delay(hi_ms);
  (void)EPD_WaitBusy_us();
  // A hardware reset drops the analog rails, so the cached flag MUST go with
  // them. This is the single choke point every reset and init path passes
  // through, which is the whole reason the flag lives here rather than in an app.
  g_analog_on = false;
}

void EPD_HW_RESET(void)
{
  EPD_HW_RESET_ms(10, 10, 10);
}

// 0xFF, not 0xF7. Per the datasheet 0x22 table, 0xF7 is "load LUT (3-color) +
// DISPLAY (3-color mode)" and 0xFF is the black/white equivalent. This panel is
// b/w, so the vendor's 0xF7 has been running a full refresh in the WRONG COLOUR
// MODE — the same mistake as 0xC7, which we measured as inverted and
// cascade-asymmetric. It leaves the panel in a state the seed does not expect.
void EPD_Update(void)
{
  EPD_WR_REG(0x22);
  EPD_WR_DATA8(0xFF);
  EPD_WR_REG(0x20);
  (void)EPD_WaitBusy_us();
  EPD_NoteSeqRails(0xFF);  // b1/b0 set: this one drops the rails on the way out
}

void EPD_PartUpdate(void)
{
  EPD_WR_REG(0x22);
  EPD_WR_DATA8(0xDC);
  EPD_WR_REG(0x20);
  (void)EPD_WaitBusy_us();
  EPD_NoteSeqRails(0xDC);  // b1/b0 CLEAR: the vendor partial leaves rails UP
}

void EPD_FastUpdate(void)
{
  EPD_WR_REG(0x22);
  EPD_WR_DATA8(0xC7);
  EPD_WR_REG(0x20);
  (void)EPD_WaitBusy_us();
  EPD_NoteSeqRails(0xC7);
}

uint32_t EPD_PartUpdate_Timed(uint32_t *busy_us_out)
{
  uint32_t t0 = micros();
  EPD_WR_REG(0x22);
  EPD_WR_DATA8(0xDC);
  EPD_WR_REG(0x20);
  uint32_t b = EPD_WaitBusy_us();
  EPD_NoteSeqRails(0xDC);
  if (busy_us_out) *busy_us_out = b;
  return micros() - t0;
}

void EPD_WriteLut(const uint8_t *ws227)
{
  EPD_WriteLutTarget(0x00, ws227);
}

void EPD_WriteLutTarget(uint8_t tgt, const uint8_t *ws227)
{
  uint16_t i;
  EPD_WR_REG((uint8_t)(0x32 | tgt));
  for (i = 0; i < EPD_LUT_WS_BYTES; i++)
    EPD_WR_DATA8(ws227[i]);
}

// mask bits: 1 = EOPT(0x3F), 2 = VGH(0x03), 4 = source(0x04), 8 = VCOM(0x2C).
// Selectable one at a time on purpose — writing them together makes it
// impossible to attribute a change to any single register.
void EPD_WriteLutVoltagesMaskTarget(uint8_t tgt, const uint8_t *ws233, uint8_t mask)
{
  if (mask & 1) {
    EPD_WR_REG((uint8_t)(0x3F | tgt));
    EPD_WR_DATA8(ws233[227]);
  }
  if (mask & 2) {
    EPD_WR_REG((uint8_t)(0x03 | tgt));
    EPD_WR_DATA8(ws233[228]);
  }
  if (mask & 4) {
    EPD_WR_REG((uint8_t)(0x04 | tgt));
    EPD_WR_DATA8(ws233[229]);
    EPD_WR_DATA8(ws233[230]);
    EPD_WR_DATA8(ws233[231]);
  }
  if (mask & 8) {
    EPD_WR_REG((uint8_t)(0x2C | tgt));
    EPD_WR_DATA8(ws233[232]);
  }
}

void EPD_WriteLutVoltagesTarget(uint8_t tgt, const uint8_t *ws233)
{
  EPD_WriteLutVoltagesMaskTarget(tgt, ws233, 0x0F);
}

void EPD_WriteDisplayOpt37Target(uint8_t tgt)
{
  static const uint8_t kOpt[10] = {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00};
  uint8_t i;
  EPD_WR_REG((uint8_t)(0x37 | tgt));
  for (i = 0; i < 10; i++)
    EPD_WR_DATA8(kOpt[i]);
}

void EPD_WriteLutFull(const uint8_t *ws233)
{
  EPD_WriteLutTarget(0x00, ws233);
  EPD_WriteLutVoltagesTarget(0x00, ws233);
}

void EPD_ReloadOtpLutSeq(uint8_t seq)
{
  // Load LUT from OTP, clock off — RAM planes untouched.
  // 0x91 = load 3-color LUT, 0x99 = load b/w LUT (datasheet 0x22 table);
  // 0xB1 / 0xB9 are the same two with "load temperature value" ahead of them.
  EPD_WR_REG(0x22);
  EPD_WR_DATA8(seq);
  EPD_WR_REG(0x20);
  delay(10);
  (void)EPD_WaitBusy_us(200);
  EPD_NoteSeqRails(seq);
}

void EPD_ReloadOtpLut(void) { EPD_ReloadOtpLutSeq(0x91); }

void EPD_UpdateCustom(void)
{
  // Cascade-friendly display control (GxEPD2 5.79)
  EPD_WR_REG(0x21);
  EPD_WR_DATA8(0x00);
  EPD_WR_DATA8(0x10);
  // 0xCF = clock+analog+DISPLAY(BW)+off — does NOT reload OTP LUT
  EPD_WR_REG(0x22);
  EPD_WR_DATA8(0xCF);
  EPD_WR_REG(0x20);
  (void)EPD_WaitBusy_us();
  EPD_NoteSeqRails(0xCF);
}

uint32_t EPD_UpdateCustom_Timed(uint32_t *busy_us_out)
{
  return EPD_UpdateSeq_Timed(0xCF, busy_us_out);
}

// Bare activate: no 0x21 write, so the ONLY difference from the proven 0xDC
// path is the sequence byte itself. Use this for custom-LUT experiments.
uint32_t EPD_UpdateSeqPlain_Timed(uint8_t seq22, uint32_t *busy_us_out)
{
  // Raise the rails BEFORE the timer starts — a byte with b6 clear cannot do it
  // itself, and folding the ~91-124 ms ramp into the measurement is what made
  // the fast path look no faster than the vendor's. Idempotent, so an app that
  // already manages rails itself pays nothing here.
  (void)EPD_EnsureRailsFor(seq22);
  uint32_t t0 = micros();
  EPD_WR_REG(0x22);
  EPD_WR_DATA8(seq22);
  EPD_WR_REG(0x20);
  uint32_t b = EPD_WaitBusy_us();
  EPD_NoteSeqRails(seq22);
  if (busy_us_out) *busy_us_out = b;
  return micros() - t0;
}

uint32_t EPD_UpdateSeq_Timed(uint8_t seq22, uint32_t *busy_us_out)
{
  (void)EPD_EnsureRailsFor(seq22);
  uint32_t t0 = micros();
  EPD_WR_REG(0x21);
  EPD_WR_DATA8(0x00);
  EPD_WR_DATA8(0x10);
  EPD_WR_REG(0x22);
  EPD_WR_DATA8(seq22);
  EPD_WR_REG(0x20);
  uint32_t b = EPD_WaitBusy_us();
  EPD_NoteSeqRails(seq22);
  if (busy_us_out) *busy_us_out = b;
  return micros() - t0;
}

void EPD_PartUpdate_OTP(void) { EPD_PartUpdate(); }

uint32_t EPD_PartUpdate_OTP_Timed(uint32_t *busy_us_out)
{
  return EPD_PartUpdate_Timed(busy_us_out);
}

uint32_t EPD_FastUpdate_Timed(uint32_t *busy_us_out)
{
  uint32_t t0 = micros();
  EPD_WR_REG(0x22);
  EPD_WR_DATA8(0xC7);
  EPD_WR_REG(0x20);
  uint32_t b = EPD_WaitBusy_us();
  EPD_NoteSeqRails(0xC7);
  if (busy_us_out) *busy_us_out = b;
  return micros() - t0;
}

uint32_t EPD_Update_Timed(uint32_t *busy_us_out)
{
  uint32_t t0 = micros();
  EPD_WR_REG(0x22);
  EPD_WR_DATA8(0xFF);  // b/w full refresh; 0xF7 is the 3-color variant
  EPD_WR_REG(0x20);
  uint32_t b = EPD_WaitBusy_us();
  EPD_NoteSeqRails(0xFF);
  if (busy_us_out) *busy_us_out = b;
  return micros() - t0;
}

void EPD_DeepSleep(void)
{
  EPD_WR_REG(0x10);
  EPD_WR_DATA8(0x01);
  delay(1);  // was 5 — minimum settle after sleep cmd
}

void EPD_Init(void)
{
  EPD_HW_RESET();
  (void)EPD_WaitBusy_us();
  EPD_WR_REG(0x12);
  (void)EPD_WaitBusy_us();
}

// load_seq picks WHICH OTP waveform gets loaded:
//   0xB1 / 0x91 = 3-color-mode LUT (what the vendor code ships, even though this
//                 is a b/w panel), 0xB9 / 0x99 = black/white-mode LUT.
// The pair is (load temp + LUT, then LUT again after the fake temperature write).
void EPD_FastMode1Init_Ex(uint8_t load_seq)
{
  uint8_t reload = (uint8_t)(load_seq & ~0x20);  // same LUT select, no temp load

  EPD_HW_RESET();
  (void)EPD_WaitBusy_us();
  EPD_WR_REG(0x12);
  (void)EPD_WaitBusy_us();

  EPD_WR_REG(0x18);
  EPD_WR_DATA8(0x80);

  EPD_WR_REG(0x22);
  EPD_WR_DATA8(load_seq);
  EPD_WR_REG(0x20);
  (void)EPD_WaitBusy_us();

  // Fake 100 C so the COG picks its hottest (shortest) waveform bank.
  EPD_WR_REG(0x1A);
  EPD_WR_DATA8(0x64);
  EPD_WR_DATA8(0x00);

  EPD_WR_REG(0x22);
  EPD_WR_DATA8(reload);
  EPD_WR_REG(0x20);
  (void)EPD_WaitBusy_us();

  EPD_WR_REG(0x3C);
  EPD_WR_DATA8(0x3);
  (void)EPD_WaitBusy_us();
}

void EPD_FastMode1Init(void) { EPD_FastMode1Init_Ex(0xB1); }

void EPD_SetRAMMP(void)
{
  static const uint8_t d11[] = {0x05};
  static const uint8_t d44[] = {0x00, 0x31};
  static const uint8_t d45[] = {0x0f, 0x01, 0x00, 0x00};
  EPD_WR_CmdData(0x11, d11, sizeof(d11));
  EPD_WR_CmdData(0x44, d44, sizeof(d44));
  EPD_WR_CmdData(0x45, d45, sizeof(d45));
}

void EPD_SetRAMMA(void)
{
  static const uint8_t d4e[] = {0x00};
  static const uint8_t d4f[] = {0x0f, 0x01};
  EPD_WR_CmdData(0x4e, d4e, sizeof(d4e));
  EPD_WR_CmdData(0x4f, d4f, sizeof(d4f));
}

void EPD_SetRAMSP(void)
{
  static const uint8_t d91[] = {0x04};
  static const uint8_t dc4[] = {0x31, 0x00};
  static const uint8_t dc5[] = {0x0f, 0x01, 0x00, 0x00};
  EPD_WR_CmdData(0x91, d91, sizeof(d91));
  EPD_WR_CmdData(0xc4, dc4, sizeof(dc4));
  EPD_WR_CmdData(0xc5, dc5, sizeof(dc5));
}

void EPD_SetRAMSA(void)
{
  static const uint8_t dce[] = {0x31};
  static const uint8_t dcf[] = {0x0f, 0x01};
  EPD_WR_CmdData(0xce, dce, sizeof(dce));
  EPD_WR_CmdData(0xcf, dcf, sizeof(dcf));
}

void EPD_Display_Clear(void)
{
  uint16_t i, j;
  EPD_SetRAMMP();
  EPD_SetRAMMA();
  EPD_WR_REG(0x24);
  for (i = 0; i < Gate_BITS; i++)
    for (j = 0; j < Source_BYTES; j++)
      EPD_WR_DATA8(0xFF);
  EPD_SetRAMMA();
  EPD_WR_REG(0x26);
  for (i = 0; i < Gate_BITS; i++)
    for (j = 0; j < Source_BYTES; j++)
      EPD_WR_DATA8(0x00);
  EPD_SetRAMSP();
  EPD_SetRAMSA();
  EPD_WR_REG(0xA4);
  for (i = 0; i < Gate_BITS; i++)
    for (j = 0; j < Source_BYTES; j++)
      EPD_WR_DATA8(0xFF);
  EPD_SetRAMSA();
  EPD_WR_REG(0xA6);
  for (i = 0; i < Gate_BITS; i++)
    for (j = 0; j < Source_BYTES; j++)
      EPD_WR_DATA8(0x00);
}

void EPD_Display(const uint8_t *ImageBW)
{
  uint32_t i;
  uint8_t tempOriginal;
  uint32_t tempcol = 0;
  uint32_t templine = 0;
  EPD_SetRAMMP();
  EPD_SetRAMMA();
  EPD_WR_REG(0x24);
  EPD_SpiBurstBegin();
  EPD_SpiBurstStrided(ImageBW, NULL, ALLSCREEN_BYTES, 1, 0);
  EPD_SpiBurstEnd();
  EPD_SetRAMSP();
  EPD_SetRAMSA();
  EPD_WR_REG(0xa4);
  EPD_SpiBurstBegin();
  EPD_SpiBurstStrided(ImageBW + ALLSCREEN_BYTES, NULL, ALLSCREEN_BYTES, 1, 0);
  EPD_SpiBurstEnd();
  (void)i; (void)tempOriginal; (void)tempcol; (void)templine;
}

uint32_t EPD_Display_Timed(const uint8_t *ImageBW, uint32_t *spi_us_out)
{
  uint32_t t0 = micros();
  EPD_Display(ImageBW);
  uint32_t dt = micros() - t0;
  if (spi_us_out) *spi_us_out = dt;
  return dt;
}

// Same scan order as EPD_Display, but into OLD image RAM (needed for clean Part diffs).
// invert=true writes ~byte — every pixel then reads as "changed" so the differential
// partial waveform drives a full transition everywhere (bb_epaper PLANE_FALSE_DIFF).
// One implementation only: the scan order must never drift from EPD_Display's.
void EPD_Display_OldPlane_Ex(const uint8_t *ImageBW, bool invert)
{
  uint32_t i;
  uint8_t tempOriginal;
  uint32_t tempcol = 0;
  uint32_t templine = 0;
  const uint8_t xc = invert ? 0xFF : 0x00;
  EPD_SetRAMMP();
  EPD_SetRAMMA();
  EPD_WR_REG(0x26);
  EPD_SpiBurstBegin();
  EPD_SpiBurstStrided(ImageBW, NULL, ALLSCREEN_BYTES, 1, xc);
  EPD_SpiBurstEnd();
  EPD_SetRAMSP();
  EPD_SetRAMSA();
  EPD_WR_REG(0xA6);
  EPD_SpiBurstBegin();
  EPD_SpiBurstStrided(ImageBW + ALLSCREEN_BYTES, NULL, ALLSCREEN_BYTES, 1, xc);
  EPD_SpiBurstEnd();
  EPD_RestoreFullWindows();
  (void)i; (void)tempOriginal; (void)tempcol; (void)templine;
}

void EPD_Display_OldPlane(const uint8_t *ImageBW)
{
  EPD_Display_OldPlane_Ex(ImageBW, false);
}

// OLD = ImageBW ^ mask. Wherever the mask bit is 1 the pixel reads as CHANGED,
// so the differential waveform drives it again even though the image did not
// move. That is the whole trick behind trail chasing: a transition that only got
// halfway is normally abandoned, because from the next frame on NEW == OLD and
// the pixel is classified unchanged forever. Re-asserting it in the mask gives
// it another full drive at NO extra waveform time — same LUT, same frame count.
// Scan order must match EPD_Display exactly; mask uses the same FB_WBYTES stride.
void EPD_Display_OldPlane_Masked(const uint8_t *ImageBW, const uint8_t *mask)
{
  uint32_t i;
  uint32_t tempcol = 0;
  uint32_t templine = 0;
  EPD_SetRAMMP();
  EPD_SetRAMMA();
  EPD_WR_REG(0x26);
  EPD_SpiBurstBegin();
  EPD_SpiBurstStrided(ImageBW, mask, ALLSCREEN_BYTES, 1, 0);
  EPD_SpiBurstEnd();
  EPD_SetRAMSP();
  EPD_SetRAMSA();
  EPD_WR_REG(0xA6);
  EPD_SpiBurstBegin();
  EPD_SpiBurstStrided(ImageBW + ALLSCREEN_BYTES, mask + ALLSCREEN_BYTES,
                      ALLSCREEN_BYTES, 1, 0);
  EPD_SpiBurstEnd();
  EPD_RestoreFullWindows();
  (void)i; (void)tempcol; (void)templine;
}

void EPD_LoadWhiteBothPlanes(void)
{
  uint16_t i, j;
  EPD_SetRAMMP();
  EPD_SetRAMMA();
  EPD_WR_REG(0x24);
  EPD_SpiBurstBegin();
  for (i = 0; i < ALLSCREEN_BYTES; i++)
    EPD_WR_DATA8_Burst(0xFF);
  EPD_SpiBurstEnd();
  EPD_SetRAMMA();
  EPD_WR_REG(0x26);
  EPD_SpiBurstBegin();
  for (i = 0; i < ALLSCREEN_BYTES; i++)
    EPD_WR_DATA8_Burst(0xFF);
  EPD_SpiBurstEnd();
  EPD_SetRAMSP();
  EPD_SetRAMSA();
  EPD_WR_REG(0xA4);
  EPD_SpiBurstBegin();
  for (i = 0; i < ALLSCREEN_BYTES; i++)
    EPD_WR_DATA8_Burst(0xFF);
  EPD_SpiBurstEnd();
  EPD_SetRAMSA();
  EPD_WR_REG(0xA6);
  EPD_SpiBurstBegin();
  for (i = 0; i < ALLSCREEN_BYTES; i++)
    EPD_WR_DATA8_Burst(0xFF);
  EPD_SpiBurstEnd();
  EPD_RestoreFullWindows();
}

// Write one chip's window. master: regs 0x44/45/4E/4F/0x24; slave: 0xC4/C5/CE/CF/0xA4
// mask != NULL: OLD gets (img ^ mask) so trail chasing survives a windowed write.
static uint32_t write_chip_window(bool master, const uint8_t *ImageBW,
                                  uint8_t c0, uint8_t c1, uint16_t r0, uint16_t r1,
                                  bool also_old, uint32_t *bytes_out,
                                  const uint8_t *mask)
{
  // Address mapping, taken from what the vendor full-screen setup actually does
  // (EPD_SetRAMMP / EPD_SetRAMSP) rather than from what looks natural:
  //
  //   master X (0x44): start 0,  end 49   -> INCREMENTS, chip col == buffer col
  //   slave  X (0xC4): start 49, end 0    -> DECREMENTS, chip col == 99 - buffer col
  //   both   Y (0x45/0xC5): start 271, end 0 -> DECREMENTS, chip row == 271 - buffer row
  //
  // A window therefore has to be expressed in mirrored chip coordinates on the Y
  // axis for both chips and on the X axis for the slave. Writing the buffer
  // coordinates straight through addresses every row backwards and the whole
  // right-hand half backwards — which looks exactly like the two controllers
  // fighting each other, with everything striped.
  // Both mirrorings are switchable at runtime. The register documentation says
  // the counters run 271->0 (Y, both chips) and 49->0 (X, slave), which implies
  // chip row == 271 - buffer row and slave chip col == 99 - buffer col. On glass
  // that produced a band correct in the middle and wrong at the extremes —
  // exactly what a band overlapping its own mirror image looks like. Rather than
  // keep arguing with the panel, both are toggles.
  uint8_t lc0, lc1;
  bool cols_desc = false;  // walk buffer columns downward instead of upward
  if (master) {
    lc0 = c0;
    lc1 = c1;
  } else if (EPD_WinXMirror()) {
    lc0 = (uint8_t)(FB_WBYTES - 1 - c0);
    lc1 = (uint8_t)(FB_WBYTES - 1 - c1);
  } else {
    lc0 = (uint8_t)(c1 - 50);
    lc1 = (uint8_t)(c0 - 50);
    cols_desc = true;
  }

  uint16_t lr0, lr1;
  bool rows_desc = false;
  if (EPD_WinYMirror()) {
    lr0 = (uint16_t)(Gate_BITS - 1 - r0);
    lr1 = (uint16_t)(Gate_BITS - 1 - r1);
  } else {
    lr0 = r1;
    lr1 = r0;
    rows_desc = true;
  }
  uint8_t buf_c0 = c0;
  uint8_t buf_c1 = c1;

  // Re-issued in full before EVERY plane, not once per window.
  //
  // Rewinding only the address counters (0x4E/0x4F) between the NEW and OLD
  // writes is not enough: it leaves OLD landing on different pixels than NEW.
  // Proven by writing OLD == NEW inside a window, which must drive NOTHING —
  // and instead drove a bar plus most of the panel.
  // Range as programmed: bounds form puts the lower value first regardless of
  // which end the counter starts from. The counter (0x4E/0x4F, 0xCE/0xCF) still
  // gets the direction-correct start in both forms.
  const uint8_t xa = (g_win_bounds && lc1 < lc0) ? lc1 : lc0;
  const uint8_t xb = (g_win_bounds && lc1 < lc0) ? lc0 : lc1;
  const uint16_t ya = (g_win_bounds && lr1 < lr0) ? lr1 : lr0;
  const uint16_t yb = (g_win_bounds && lr1 < lr0) ? lr0 : lr1;

  auto set_window = [&]() {
    if (master) {
      EPD_WR_REG(0x11);
      EPD_WR_DATA8(0x05);
      EPD_WR_REG(0x44);
      EPD_WR_DATA8(xa);
      EPD_WR_DATA8(xb);
      EPD_WR_REG(0x45);
      EPD_WR_DATA8((uint8_t)(ya & 0xFF));
      EPD_WR_DATA8((uint8_t)(ya >> 8));
      EPD_WR_DATA8((uint8_t)(yb & 0xFF));
      EPD_WR_DATA8((uint8_t)(yb >> 8));
      EPD_WR_REG(0x4E);
      EPD_WR_DATA8(lc0);  // counter start follows the scan direction
      EPD_WR_REG(0x4F);
      EPD_WR_DATA8((uint8_t)(lr0 & 0xFF));
      EPD_WR_DATA8((uint8_t)(lr0 >> 8));
    } else {
      EPD_WR_REG(0x91);
      EPD_WR_DATA8(0x04);
      EPD_WR_REG(0xC4);
      EPD_WR_DATA8(xa);
      EPD_WR_DATA8(xb);
      EPD_WR_REG(0xC5);
      EPD_WR_DATA8((uint8_t)(ya & 0xFF));
      EPD_WR_DATA8((uint8_t)(ya >> 8));
      EPD_WR_DATA8((uint8_t)(yb & 0xFF));
      EPD_WR_DATA8((uint8_t)(yb >> 8));
      EPD_WR_REG(0xCE);
      EPD_WR_DATA8(lc0);
      EPD_WR_REG(0xCF);
      EPD_WR_DATA8((uint8_t)(lr0 & 0xFF));
      EPD_WR_DATA8((uint8_t)(lr0 >> 8));
    }
  };

  set_window();

  uint32_t nbytes = 0;
  uint32_t t0 = micros();

  // Traversal order must follow whichever direction the counters were set to,
  // or the data lands reversed inside an otherwise-correct window.
  auto pump = [&](uint8_t reg, bool use_mask) {
    EPD_WR_REG(reg);
    EPD_SpiBurstBegin();
    for (int ci = 0; ci <= (int)(buf_c1 - buf_c0); ci++) {
      uint8_t c = cols_desc ? (uint8_t)(buf_c1 - ci) : (uint8_t)(buf_c0 + ci);
      for (int ri = 0; ri <= (int)(r1 - r0); ri++) {
        uint16_t r = rows_desc ? (uint16_t)(r1 - ri) : (uint16_t)(r0 + ri);
        uint32_t off = FB_IDX(r, c);
        uint8_t v = ImageBW[off];
        if (use_mask && mask) v ^= mask[off];
        EPD_WR_DATA8_Burst(v);
        nbytes++;
      }
    }
    EPD_SpiBurstEnd();
  };

  if (master) {
    pump(0x24, false);
    if (also_old) {
      set_window();
      pump(0x26, true);
    }
  } else {
    pump(0xA4, false);
    if (also_old) {
      set_window();
      pump(0xA6, true);
    }
  }

  uint32_t dt = micros() - t0;
  if (bytes_out) *bytes_out += nbytes;
  return dt;
}

uint32_t EPD_WriteWindowMasked(const uint8_t *ImageBW, const uint8_t *mask,
                               uint8_t c0, uint8_t c1, uint16_t r0, uint16_t r1,
                               bool also_old_ram, uint32_t *bytes_out)
{
  if (c0 > c1 || r0 > r1 || c1 >= FB_WBYTES || r1 >= Gate_BITS)
    return 0;

  uint32_t bytes = 0;
  uint32_t t0 = micros();

  // Master portion
  if (c0 < 50) {
    uint8_t mc0 = c0;
    uint8_t mc1 = (c1 < 50) ? c1 : 49;
    write_chip_window(true, ImageBW, mc0, mc1, r0, r1, also_old_ram, &bytes, mask);
  }
  // Slave portion
  if (c1 >= 50) {
    uint8_t sc0 = (c0 >= 50) ? c0 : 50;
    uint8_t sc1 = c1;
    write_chip_window(false, ImageBW, sc0, sc1, r0, r1, also_old_ram, &bytes, mask);
  }

  // Critical: restore full address windows so later EPD_Display / UI paint
  // does not hang or write into a leftover 24x24 window.
  EPD_RestoreFullWindows();

  if (bytes_out) *bytes_out = bytes;
  return micros() - t0;
}

uint32_t EPD_WriteWindow(const uint8_t *ImageBW, uint8_t c0, uint8_t c1,
                         uint16_t r0, uint16_t r1, bool also_old_ram,
                         uint32_t *bytes_out)
{
  return EPD_WriteWindowMasked(ImageBW, NULL, c0, c1, r0, r1, also_old_ram, bytes_out);
}
