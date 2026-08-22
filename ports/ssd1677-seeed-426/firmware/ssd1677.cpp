#include "ssd1677.h"

#include <string.h>

static bool g_busy_to = false;
static bool g_analog_on = false;

bool EPD_BusyTimedOut(void) { return g_busy_to; }
bool EPD_AnalogIsOn(void) { return g_analog_on; }

uint32_t EPD_WaitBusy_us(uint32_t timeout_ms)
{
  g_busy_to = false;
  uint32_t t0 = micros();
  uint32_t limit = timeout_ms * 1000UL;
  if (limit < 1000) limit = 1000;
  while (EPD_ReadBUSY != 0) {
    if ((micros() - t0) > limit) {
      g_busy_to = true;
      break;
    }
    yield();  // hundreds of ms of spinning would otherwise trip the task WDT
  }
  return micros() - t0;
}

void EPD_Reset(void)
{
  EPD_RES_Set();
  delay(10);
  EPD_RES_Clr();
  delay(10);  // 10 ms low, per the panel spec
  EPD_RES_Set();
  delay(10);
  (void)EPD_WaitBusy_us(1000);
}

// Shared head of both init paths: soft reset, temperature sensor, booster soft
// start, gate count and border. Values follow the GxEPD2 sequence for this
// panel, which is the one known to work on the real glass.
static void init_common(void)
{
  EPD_GPIOInit();
  EPD_Reset();
  // A hardware reset drops the analog rails, so the cached flag MUST be cleared
  // with them. EPD_Update() skips powering the rails for the bare 0x0C fast path
  // when it believes they are already up — leave this stale and every subsequent
  // DISPLAY silently does nothing, with busy_ms=0 the only visible symptom.
  g_analog_on = false;

  EPD_WR_REG(0x12);  // SWRESET
  delay(10);
  (void)EPD_WaitBusy_us(1000);

  EPD_WR_REG(0x18);  // temperature sensor: use the internal one
  EPD_WR_DATA8(0x80);

  EPD_WR_REG(0x0C);  // booster soft start
  EPD_WR_DATA8(0xAE);
  EPD_WR_DATA8(0xC7);
  EPD_WR_DATA8(0xC3);
  EPD_WR_DATA8(0xC0);
  EPD_WR_DATA8(0x80);

  EPD_WR_REG(0x01);  // driver output control: gate count = H-1
  EPD_WR_DATA8((uint8_t)((EPD_H - 1) & 0xFF));
  EPD_WR_DATA8((uint8_t)((EPD_H - 1) >> 8));
  EPD_WR_DATA8(0x02);  // SM: interlaced gate scan, as the vendor sequence uses

  EPD_WR_REG(0x3C);  // border waveform
  EPD_WR_DATA8(0x01);

  EPD_SetRamFull();
  (void)EPD_WaitBusy_us(1000);
}

// Gate count via 0x01. A waveform "frame" is one scan of every gate line, so the
// frame period should fall in proportion to this — the lever that makes a short
// band far faster than a full panel, rather than merely cheaper to transfer.
// Lines outside the count are not scanned and simply hold their image.
void EPD_SetGateCount(uint16_t lines)
{
  if (lines < 1) lines = 1;
  if (lines > EPD_H) lines = EPD_H;
  EPD_WR_REG(0x01);
  EPD_WR_DATA8((uint8_t)((lines - 1) & 0xFF));
  EPD_WR_DATA8((uint8_t)((lines - 1) >> 8));
  EPD_WR_DATA8(0x02);  // SM: interlaced gate scan, matching init_common
}

void EPD_Init(void) { init_common(); }

void EPD_InitFast(void)
{
  init_common();
  // Claim 90 C so the COG selects its hottest — and therefore shortest —
  // factory waveform. Exactly the trick that works on the 5.79 (which uses
  // 0x64 = 100 C); 0x5A is what this panel's vendor code uses.
  EPD_WR_REG(0x1A);
  EPD_WR_DATA8(0x5A);
  EPD_WR_REG(0x22);
  EPD_WR_DATA8(0x91);  // load LUT with Display Mode 1
  EPD_WR_REG(0x20);
  (void)EPD_WaitBusy_us(2000);
}

// x/y/w/h in PIXELS. The gates are physically reversed and the controller has
// no reverse-scan bit, so Y is flipped here and programmed decreasing; rows are
// then written 0..h-1 in natural order.
//
// ONLY EVER CALLED FULL-PANEL, and only that case is verified. Sub-windows do
// not land where they should — see docs/PLAYER-ROADMAP.md. The prime suspect is
// that 0x44/0x4E address 8-pixel BYTE groups rather than pixels, which a
// full-panel write cannot distinguish because x = 0 is 0 either way and the write
// simply runs to the end of the row. Writing bytes here was tried and did not fix
// it, so something else is wrong as well; settle it from the datasheet before
// trusting a window.
void EPD_SetRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
  uint16_t yr = (uint16_t)(EPD_H - y - h);

  EPD_WR_REG(0x11);    // data entry mode
  EPD_WR_DATA8(0x01);  // X increment, Y decrement, X is the fast axis

  EPD_WR_REG(0x44);  // X start/end
  EPD_WR_DATA8((uint8_t)(x & 0xFF));
  EPD_WR_DATA8((uint8_t)(x >> 8));
  EPD_WR_DATA8((uint8_t)((x + w - 1) & 0xFF));
  EPD_WR_DATA8((uint8_t)((x + w - 1) >> 8));

  EPD_WR_REG(0x45);  // Y start/end — start is the HIGH value, Y decrements
  EPD_WR_DATA8((uint8_t)((yr + h - 1) & 0xFF));
  EPD_WR_DATA8((uint8_t)((yr + h - 1) >> 8));
  EPD_WR_DATA8((uint8_t)(yr & 0xFF));
  EPD_WR_DATA8((uint8_t)(yr >> 8));

  EPD_WR_REG(0x4E);  // X counter
  EPD_WR_DATA8((uint8_t)(x & 0xFF));
  EPD_WR_DATA8((uint8_t)(x >> 8));

  EPD_WR_REG(0x4F);  // Y counter starts at the high end
  EPD_WR_DATA8((uint8_t)((yr + h - 1) & 0xFF));
  EPD_WR_DATA8((uint8_t)((yr + h - 1) >> 8));
}

void EPD_SetRamFull(void) { EPD_SetRamArea(0, 0, EPD_W, EPD_H); }

// The framebuffer is row-major and each row is exactly EPD_WBYTES, and X is the
// fast axis — so a full-panel write is one contiguous run. No transpose, no
// stride, straight into the DMA buffer.
static void write_plane(uint8_t reg, const uint8_t *fb, const uint8_t *mask,
                        uint8_t xconst)
{
  EPD_SetRamFull();
  EPD_WR_REG(reg);
  EPD_SpiBurstBegin();
  EPD_SpiBurstStrided(fb, mask, EPD_PLANE_BYTES, 1, xconst);
  EPD_SpiBurstEnd();
}

// Same, but into a window: `fb` is w/8 bytes per row, h rows, packed tight.
//
// The UPDATE still scans every gate line — frame period is fixed by the
// frame-rate register and does not fall with the gate count (measured: 9921 us
// at MUX 480, 240, 120 and 60 alike). Pixels outside the window are simply left
// with old == new in RAM, so the waveform gives them no drive and they hold.
// The saving here is transfer time only, not waveform time.
void EPD_WriteRegion(uint8_t reg, const uint8_t *fb, uint16_t x, uint16_t y,
                     uint16_t w, uint16_t h)
{
  EPD_SetRamArea(x, y, w, h);
  EPD_WR_REG(reg);
  EPD_SpiBurstBegin();
  EPD_SpiBurstStrided(fb, nullptr, (uint32_t)(w / 8) * h, 1, 0x00);
  EPD_SpiBurstEnd();
}

void EPD_WriteNew(const uint8_t *fb) { write_plane(0x24, fb, nullptr, 0x00); }

void EPD_WriteOld(const uint8_t *fb, bool invert)
{
  write_plane(0x26, fb, nullptr, invert ? 0xFF : 0x00);
}

void EPD_WriteOldMasked(const uint8_t *fb, const uint8_t *mask)
{
  write_plane(0x26, fb, mask, 0x00);
}

void EPD_FillNew(uint8_t value)
{
  EPD_SetRamFull();
  EPD_WR_REG(0x24);
  EPD_SpiBurstBegin();
  for (uint32_t i = 0; i < EPD_PLANE_BYTES; i++) EPD_WR_DATA8_Burst(value);
  EPD_SpiBurstEnd();
}

void EPD_SetRamOption(uint8_t old_opt, uint8_t new_opt)
{
  EPD_WR_REG(0x21);
  EPD_WR_DATA8((uint8_t)((old_opt << 4) | (new_opt & 0x0F)));
  EPD_WR_DATA8(0x00);  // single-chip application
}

void EPD_AnalogPower(bool on)
{
  EPD_WR_REG(0x22);
  EPD_WR_DATA8(on ? 0xC0 : 0x03);
  EPD_WR_REG(0x20);
  (void)EPD_WaitBusy_us(2000);
  g_analog_on = on;
}

uint32_t EPD_Update(uint8_t seq22, uint32_t *busy_us_out)
{
  // Bytes with b6 clear do not power the analog block themselves, so the rails
  // must already be up. Bytes with b1/b0 set tear them down again.
  if (!(seq22 & 0x40) && !g_analog_on) EPD_AnalogPower(true);
  if (seq22 & 0x03) g_analog_on = false;

  uint32_t t0 = micros();
  EPD_WR_REG(0x22);
  EPD_WR_DATA8(seq22);
  EPD_WR_REG(0x20);
  uint32_t b = EPD_WaitBusy_us();
  if (busy_us_out) *busy_us_out = b;
  return micros() - t0;
}

uint32_t EPD_UpdateFullFast(uint32_t *busy_us_out)
{
  EPD_SetRamOption(RAM_BYPASS0, RAM_NORMAL);  // OLD treated as 0 for a full paint
  EPD_WR_REG(0x1A);
  EPD_WR_DATA8(0x5A);
  return EPD_Update(0xD7, busy_us_out);
}

uint32_t EPD_UpdatePartial(uint32_t *busy_us_out)
{
  EPD_SetRamOption(RAM_NORMAL, RAM_NORMAL);
  return EPD_Update(0xFC, busy_us_out);
}

void EPD_WriteLut(const uint8_t *lut105)
{
  EPD_WR_REG(0x32);
  for (uint16_t i = 0; i < EPD_LUT_BYTES; i++) EPD_WR_DATA8(lut105[i]);
}

void EPD_WriteVoltages(uint8_t vgh, uint8_t vsh1, uint8_t vsh2, uint8_t vsl,
                       uint8_t vcom)
{
  EPD_WR_REG(0x03);
  EPD_WR_DATA8(vgh);
  EPD_WR_REG(0x04);
  EPD_WR_DATA8(vsh1);
  EPD_WR_DATA8(vsh2);
  EPD_WR_DATA8(vsl);
  EPD_WR_REG(0x2C);
  EPD_WR_DATA8(vcom);
}
