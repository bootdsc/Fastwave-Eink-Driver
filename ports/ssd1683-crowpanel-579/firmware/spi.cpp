#include "spi.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include <string.h>

static EpdSpiPath g_path = EPD_SPI_BITBANG;
static uint32_t g_hw_hz = 20000000;
static spi_device_handle_t g_dev = nullptr;
static bool g_bus_ready = false;
static bool g_burst = false;

// Batched, double-buffered, overlapped TX.
//
// Stage 1 was one spi_transaction_t PER BYTE — 27200 transactions per frame,
// each costing driver overhead, which made HW SPI slower than bit-bang.
//
// Stage 2 batched into one DMA buffer but used spi_device_polling_transmit,
// which BLOCKS: the CPU filled a buffer, then idled while DMA drained it, then
// filled the next. Wall time was CPU + wire instead of max(CPU, wire), which is
// why 20 MHz delivered only ~1.07 MB/s of the 2.5 MB/s the clock allows.
//
// Stage 3 (here): two DMA buffers and queued transactions. The CPU fills B while
// DMA is still sending A, so the transfer costs whichever of the two is slower
// rather than their sum.
#define TXBUF_SZ 8192
static uint8_t *g_txbuf[2] = {nullptr, nullptr};
static spi_transaction_t g_tx[2];
static uint8_t g_cur = 0;
static size_t g_txlen = 0;
static int g_inflight = 0;

// Block until every queued transfer has actually gone out. Required before any
// DC change or CS release, or a command can overtake data still on the wire.
static void hw_drain(void)
{
  while (g_inflight > 0) {
    spi_transaction_t *r = nullptr;
    spi_device_get_trans_result(g_dev, &r, portMAX_DELAY);
    g_inflight--;
  }
}

static void hw_flush(void)
{
  if (!g_txlen || !g_dev || !g_txbuf[g_cur]) { g_txlen = 0; return; }

  // Queue FIRST, reap afterwards.
  //
  // The obvious order — wait for the outstanding transfer, then queue the next —
  // looks like double buffering and is not: it leaves only ever one transfer in
  // flight and inserts a full stall between every chunk, so the bus goes idle
  // between bursts. Measured 39 ms for 54400 bytes where the 20 MHz wire needs
  // 22 ms. Queueing before reaping keeps a transfer on the wire while the
  // previous one is retired and the next buffer is filled.
  spi_transaction_t *t = &g_tx[g_cur];
  memset(t, 0, sizeof(*t));
  t->length = g_txlen * 8;
  t->tx_buffer = g_txbuf[g_cur];
  spi_device_polling_transmit(g_dev, t);

  g_cur ^= 1;
  g_txlen = 0;
}

void EPD_GPIOInit(void)
{
  // Only touch non-SPI control pins when HW SPI owns SCK/MOSI
  pinMode(RES, OUTPUT);
  pinMode(DC, OUTPUT);
  pinMode(CS, OUTPUT);
  pinMode(BUSY, INPUT);
  EPD_CS_Set();

  if (g_path == EPD_SPI_BITBANG) {
    pinMode(SCK, OUTPUT);
    pinMode(MOSI, OUTPUT);
    EPD_SCK_Clr();
  }
}

static void hw_teardown(void)
{
  if (g_dev) { hw_drain(); spi_device_release_bus(g_dev); }
  g_txlen = 0;
  if (g_dev) {
    spi_bus_remove_device(g_dev);
    g_dev = nullptr;
  }
  if (g_bus_ready) {
    spi_bus_free(SPI2_HOST);
    g_bus_ready = false;
  }
  // Return pins to GPIO for bitbang
  gpio_reset_pin((gpio_num_t)SCK);
  gpio_reset_pin((gpio_num_t)MOSI);
  pinMode(SCK, OUTPUT);
  pinMode(MOSI, OUTPUT);
  EPD_SCK_Clr();
}

static bool hw_ensure(void)
{
  if (g_dev) return true;

  // Free pins from GPIO driver before SPI takes them
  gpio_reset_pin((gpio_num_t)SCK);
  gpio_reset_pin((gpio_num_t)MOSI);

  for (int i = 0; i < 2; i++) {
    if (!g_txbuf[i]) {
      g_txbuf[i] = (uint8_t *)heap_caps_malloc(TXBUF_SZ, MALLOC_CAP_DMA);
      if (!g_txbuf[i]) {
        g_path = EPD_SPI_BITBANG;
        return false;
      }
    }
  }
  g_cur = 0;
  g_txlen = 0;
  g_inflight = 0;

  if (!g_bus_ready) {
    spi_bus_config_t bus = {};
    bus.mosi_io_num = MOSI;
    bus.miso_io_num = -1;
    bus.sclk_io_num = SCK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = TXBUF_SZ;
    esp_err_t e = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
      g_path = EPD_SPI_BITBANG;
      pinMode(SCK, OUTPUT);
      pinMode(MOSI, OUTPUT);
      return false;
    }
    g_bus_ready = true;
  }

  spi_device_interface_config_t dev = {};
  dev.mode = 0;
  dev.clock_speed_hz = (int)g_hw_hz;
  dev.spics_io_num = -1;
  dev.queue_size = 2;  // one in flight while the next is being filled
  dev.flags = 0;
  esp_err_t e = spi_bus_add_device(SPI2_HOST, &dev, &g_dev);
  if (e != ESP_OK) {
    g_dev = nullptr;
    g_path = EPD_SPI_BITBANG;
    hw_teardown();
    return false;
  }

  // Hold the bus for the lifetime of the device. spi_device_polling_transmit
  // otherwise acquires and releases it on EVERY call, and a frame makes ~40 tiny
  // register transfers — measured at ~16 ms of the 39 ms frame, against 22.5 ms
  // of actual data. Nothing else shares SPI2 here.
  // Queued transactions are not permitted while the bus is held, so the bulk
  // path below switches to polling as well; the benchmark showed the DMA path
  // already runs at 97.5% of wire rate, so the lost CPU/DMA overlap costs
  // nothing next to what the acquire/release churn was costing.
  spi_device_acquire_bus(g_dev, portMAX_DELAY);
  return true;
}

void EPD_SetSpiPath(EpdSpiPath path)
{
  if (path == EPD_SPI_BITBANG) {
    if (g_path == EPD_SPI_HW)
      hw_teardown();
    g_path = EPD_SPI_BITBANG;
    pinMode(SCK, OUTPUT);
    pinMode(MOSI, OUTPUT);
    EPD_SCK_Clr();
    return;
  }

  // HW path
  if (g_dev && g_path == EPD_SPI_HW) {
    g_path = EPD_SPI_HW;
    return;
  }
  g_path = EPD_SPI_HW;
  if (!hw_ensure()) {
    g_path = EPD_SPI_BITBANG;
  }
}

EpdSpiPath EPD_GetSpiPath(void) { return g_path; }

void EPD_SetHwSpiHz(uint32_t hz)
{
  if (hz < 1000000) hz = 1000000;
  if (hz > 40000000) hz = 40000000;
  if (hz == g_hw_hz && g_dev) return;
  g_hw_hz = hz;
  if (g_path == EPD_SPI_HW) {
    // Recreate the device at the new clock. Drain and RELEASE first: hw_ensure()
    // acquires the bus for the device's lifetime, and spi_bus_remove_device()
    // asserts if the device still holds it. Without this, changing the clock
    // panicked the board into a boot loop — at ANY speed, not just above spec.
    // hw_teardown() has always done it in this order; this path did not.
    if (g_dev) {
      hw_drain();
      spi_device_release_bus(g_dev);
      spi_bus_remove_device(g_dev);
      g_dev = nullptr;
    }
    if (!hw_ensure())
      g_path = EPD_SPI_BITBANG;
  }
}

uint32_t EPD_GetHwSpiHz(void) { return g_hw_hz; }

const char *EPD_SpiPathName(void)
{
  static char buf[28];
  if (g_path == EPD_SPI_BITBANG)
    return "BitBang";
  snprintf(buf, sizeof(buf), "HW %luM", (unsigned long)(g_hw_hz / 1000000UL));
  return buf;
}

static void hw_tx_byte(uint8_t dat)
{
  if (!g_dev) return;
  hw_drain();  // single bytes are commands/params — never reorder them past data
  spi_transaction_t t = {};
  t.length = 8;
  t.flags = SPI_TRANS_USE_TXDATA;
  t.tx_data[0] = dat;
  spi_device_polling_transmit(g_dev, &t);
}

void EPD_SpiBurstBegin(void)
{
  g_burst = true;
  g_txlen = 0;
  EPD_DC_Set();  // burst payload is always data; DC held high for the run
  EPD_CS_Clr();
}

void EPD_SpiBurstEnd(void)
{
  if (g_path == EPD_SPI_HW && g_dev) {
    hw_flush();
    hw_drain();  // CS must not rise while bytes are still queued
  }
  EPD_CS_Set();
  g_burst = false;
}

void EPD_WR_Bus(uint8_t dat)
{
  if (g_path == EPD_SPI_HW && g_dev) {
    if (!g_burst) EPD_CS_Clr();
    hw_tx_byte(dat);
    if (!g_burst) EPD_CS_Set();
    return;
  }

  // Bit-bang
  if (!g_burst) EPD_CS_Clr();
  for (uint8_t i = 0; i < 8; i++) {
    EPD_SCK_Clr();
    if (dat & 0x80)
      EPD_MOSI_Set();
    else
      EPD_MOSI_Clr();
    EPD_SCK_Set();
    dat <<= 1;
  }
  if (!g_burst) EPD_CS_Set();
}

void EPD_WR_REG(uint8_t reg)
{
  // A command must never overtake staged data still sitting in the TX buffer or
  // already queued to the DMA engine.
  if (g_path == EPD_SPI_HW && g_dev) {
    if (g_txlen) hw_flush();
    hw_drain();
  }
  EPD_DC_Clr();
  EPD_WR_Bus(reg);
  EPD_DC_Set();
}

void EPD_WR_DATA8(uint8_t dat)
{
  EPD_DC_Set();
  EPD_WR_Bus(dat);
}

// One command plus its parameters, in two SPI transactions instead of one per
// byte. The RAM-window setup runs four times a frame and was ~64 single-byte
// polling transfers, each with its own driver round trip and DMA drain.
void EPD_WR_CmdData(uint8_t cmd, const uint8_t *data, uint8_t n)
{
  if (g_path == EPD_SPI_HW && g_dev) {
    if (g_txlen) hw_flush();
    hw_drain();

    EPD_CS_Clr();
    EPD_DC_Clr();
    spi_transaction_t c = {};
    c.length = 8;
    c.flags = SPI_TRANS_USE_TXDATA;
    c.tx_data[0] = cmd;
    spi_device_polling_transmit(g_dev, &c);

    if (n) {
      EPD_DC_Set();
      spi_transaction_t d = {};
      d.length = (size_t)n * 8;
      if (n <= 4) {
        d.flags = SPI_TRANS_USE_TXDATA;
        memcpy(d.tx_data, data, n);
      } else {
        memcpy(g_txbuf[g_cur], data, n);
        d.tx_buffer = g_txbuf[g_cur];
      }
      spi_device_polling_transmit(g_dev, &d);
    }
    EPD_CS_Set();
    EPD_DC_Set();
    return;
  }

  EPD_WR_REG(cmd);
  for (uint8_t i = 0; i < n; i++) EPD_WR_DATA8(data[i]);
}

// Bulk strided copy straight into the DMA buffer.
//
// The panel wants column-major bytes while the framebuffer is row-major, so each
// byte is a stride-FB_WBYTES read. Doing that one byte at a time through
// EPD_WR_DATA8_Burst cost ~110 cycles per byte — a multiply, several branches
// and a non-inlined call — which dwarfed the 22 ms the 20 MHz wire actually
// needs. Here the whole column is copied by a tight pointer loop with the checks
// hoisted out, so the CPU stops being the bottleneck.
//
// mask may be NULL; when present it is XORed per byte with the same stride,
// which is what the OLD plane needs for trail chasing.
void EPD_SpiBurstStrided(const uint8_t *src, const uint8_t *mask, uint32_t count,
                         uint32_t stride, uint8_t xconst)
{
  if (g_path == EPD_SPI_HW && g_dev && g_txbuf[g_cur]) {
    while (count) {
      if (g_txlen >= TXBUF_SZ) hw_flush();
      uint32_t room = (uint32_t)(TXBUF_SZ - g_txlen);
      uint32_t n = count < room ? count : room;
      uint8_t *dst = g_txbuf[g_cur] + g_txlen;
      // With the framebuffer in the panel's own column-major order a whole chip
      // half is contiguous, so this is the path that actually runs: a straight
      // memcpy at a few bytes per cycle instead of a byte-at-a-time loop.
      if (stride == 1 && !mask && !xconst) {
        memcpy(dst, src, n);
        src += n;
      } else if (stride == 1 && mask && !xconst) {
        for (uint32_t i = 0; i < n; i++) dst[i] = (uint8_t)(src[i] ^ mask[i]);
        src += n;
        mask += n;
      } else if (mask) {
        for (uint32_t i = 0; i < n; i++) {
          dst[i] = (uint8_t)(*src ^ *mask ^ xconst);
          src += stride;
          mask += stride;
        }
      } else if (xconst) {
        for (uint32_t i = 0; i < n; i++) {
          dst[i] = (uint8_t)(*src ^ xconst);
          src += stride;
        }
      } else {
        for (uint32_t i = 0; i < n; i++) {
          dst[i] = *src;
          src += stride;
        }
      }
      g_txlen += n;
      count -= n;
    }
    return;
  }

  for (uint32_t i = 0; i < count; i++) {
    uint8_t v = (uint8_t)(*src ^ xconst);
    if (mask) { v ^= *mask; mask += stride; }
    EPD_WR_DATA8_Burst(v);
    src += stride;
  }
}

void EPD_WR_DATA8_Burst(uint8_t dat)
{
  if (g_path == EPD_SPI_HW && g_dev && g_txbuf[g_cur]) {
    if (g_txlen >= TXBUF_SZ) hw_flush();  // CS stays low across the flush
    g_txbuf[g_cur][g_txlen++] = dat;
    return;
  }
  EPD_DC_Set();
  EPD_WR_Bus(dat);
}
