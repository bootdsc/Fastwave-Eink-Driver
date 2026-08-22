#include "ssd1681.h"

#include <string.h>

static bool g_busy_to = false;
static bool g_analog_on = false;

bool EPD_BusyTimedOut(void) { return g_busy_to; }
bool EPD_AnalogIsOn(void) { return g_analog_on; }

uint32_t EPD_WaitBusy_us(uint32_t timeout_ms) {
  g_busy_to = false;
  uint32_t t0 = micros();
  uint32_t limit = timeout_ms * 1000UL;
  if (limit < 1000) limit = 1000;
  while (EPD_ReadBUSY != 0) {
    if ((micros() - t0) > limit) {
      g_busy_to = true;
      break;
    }
    yield();
  }
  return micros() - t0;
}

void EPD_Reset(void) {
  EPD_RES_Set();
  delay(10);
  EPD_RES_Clr();
  delay(10);
  EPD_RES_Set();
  delay(10);
  (void)EPD_WaitBusy_us(1000);
}

void EPD_SetRamFull(void) {
  // SSD1681 X window is in bytes (0..24), Y in pixels (0..199).
  EPD_WR_REG(0x11);
  EPD_WR_DATA8(0x03);  // X++, Y++
  EPD_WR_REG(0x44);
  EPD_WR_DATA8(0x00);
  EPD_WR_DATA8((uint8_t)(EPD_WBYTES - 1));
  EPD_WR_REG(0x45);
  EPD_WR_DATA8(0x00);
  EPD_WR_DATA8(0x00);
  EPD_WR_DATA8((uint8_t)((EPD_H - 1) & 0xFF));
  EPD_WR_DATA8((uint8_t)((EPD_H - 1) >> 8));
  EPD_WR_REG(0x4E);
  EPD_WR_DATA8(0x00);
  EPD_WR_REG(0x4F);
  EPD_WR_DATA8(0x00);
  EPD_WR_DATA8(0x00);
}

void EPD_Init(void) {
  EPD_GPIOInit();
  EPD_Reset();
  g_analog_on = false;

  EPD_WR_REG(0x12);
  delay(10);
  (void)EPD_WaitBusy_us(1000);

  EPD_WR_REG(0x01);  // gate count 200-1 = 0xC7
  EPD_WR_DATA8(0xC7);
  EPD_WR_DATA8(0x00);
  EPD_WR_DATA8(0x00);

  EPD_WR_REG(0x3C);
  EPD_WR_DATA8(0x05);

  EPD_WR_REG(0x18);
  EPD_WR_DATA8(0x80);

  EPD_SetRamFull();
  (void)EPD_WaitBusy_us(1000);
}

static void write_plane(uint8_t reg, const uint8_t *fb, const uint8_t *mask,
                        uint8_t xconst) {
  EPD_SetRamFull();
  EPD_WR_REG(reg);
  EPD_SpiBurstBegin();
  EPD_SpiBurstStrided(fb, mask, EPD_PLANE_BYTES, 1, xconst);
  EPD_SpiBurstEnd();
}

void EPD_WriteNew(const uint8_t *fb) { write_plane(0x24, fb, nullptr, 0x00); }

void EPD_WriteOld(const uint8_t *fb, bool invert) {
  write_plane(0x26, fb, nullptr, invert ? 0xFF : 0x00);
}

void EPD_WriteOldMasked(const uint8_t *fb, const uint8_t *mask) {
  write_plane(0x26, fb, mask, 0x00);
}

void EPD_SetRamOption(uint8_t old_opt, uint8_t new_opt) {
  EPD_WR_REG(0x21);
  EPD_WR_DATA8((uint8_t)((old_opt << 4) | (new_opt & 0x0F)));
  EPD_WR_DATA8(0x00);
}

void EPD_AnalogPower(bool on) {
  EPD_WR_REG(0x22);
  EPD_WR_DATA8(on ? 0xC0 : 0x03);
  EPD_WR_REG(0x20);
  (void)EPD_WaitBusy_us(2000);
  g_analog_on = on;
}

void EPD_UpdateKick(uint8_t seq22) {
  // 0x22 bits: 7 clock on, 6 analog on, 5 load temperature, 4 load LUT mode 1,
  // 3 load LUT mode 2, 2 display, 1 analog off, 0 clock off.
  // A sequence with b6 brings its own rails up; one without needs them already
  // up (the rails-up fast path). b1 is what drops them again -- b0 only stops
  // the oscillator, so keying the flag off (seq & 0x03) called analog dead
  // whenever the clock was parked.
  if (!(seq22 & 0x40) && !g_analog_on) EPD_AnalogPower(true);
  if (seq22 & 0x40) g_analog_on = true;
  if (seq22 & 0x02) g_analog_on = false;
  EPD_WR_REG(0x22);
  EPD_WR_DATA8(seq22);
  EPD_WR_REG(0x20);
}

uint32_t EPD_Update(uint8_t seq22, uint32_t *busy_us_out) {
  uint32_t t0 = micros();
  EPD_UpdateKick(seq22);
  uint32_t b = EPD_WaitBusy_us();
  if (busy_us_out) *busy_us_out = b;
  return micros() - t0;
}

void EPD_WriteLut(const uint8_t *lut153) {
  if (!lut153) return;
  EPD_WR_REG(0x32);
  EPD_SpiBurstBegin();
  EPD_SpiBurstStrided(lut153, nullptr, 153u, 1, 0x00);
  EPD_SpiBurstEnd();
}

void EPD_WriteVoltages(uint8_t vgh, uint8_t vsh1, uint8_t vsh2, uint8_t vsl,
                       uint8_t vcom) {
  if (vgh) {
    EPD_WR_REG(0x03);
    EPD_WR_DATA8(vgh);
  }
  if (vsh1 || vsh2 || vsl) {
    // 0x04 is one three-byte register; all three go together or not at all.
    EPD_WR_REG(0x04);
    EPD_WR_DATA8(vsh1 ? vsh1 : 0x41);  // POR 15 V
    EPD_WR_DATA8(vsh2 ? vsh2 : 0xA8);  // POR 5 V, low range
    EPD_WR_DATA8(vsl ? vsl : 0x32);    // POR -15 V
  }
  if (vcom) {
    // 0x08 = -0.2 V. On the SSD1683 this alone cured whole-panel DC darkening
    // under a short waveform, for zero frames and zero milliseconds. 0x00 is
    // NOT a defined setting on that part -- the register starts at 0x08.
    EPD_WR_REG(0x2C);
    EPD_WR_DATA8(vcom);
  }
}

void EPD_WriteEopt(uint8_t eopt) {
  EPD_WR_REG(0x3F);
  EPD_WR_DATA8(eopt);
}

void EPD_WriteLineTiming(uint8_t dummy_line, uint8_t gate_width) {
  if (dummy_line) {
    EPD_WR_REG(0x3A);
    EPD_WR_DATA8(dummy_line);
  }
  if (gate_width) {
    EPD_WR_REG(0x3B);
    EPD_WR_DATA8(gate_width);
  }
}
