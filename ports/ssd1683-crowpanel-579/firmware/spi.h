#ifndef _SPI_H_
#define _SPI_H_

#include <Arduino.h>
#include "soc/gpio_struct.h"

// CrowPanel 5.79" EPD pins
#define SCK  12
#define MOSI 11
#define RES  47
#define DC   46
#define CS   45
#define BUSY 48

// Fast GPIO bit-bang helpers (ESP32-S3)
#define EPD_SCK_Clr()  do { GPIO.out_w1tc = (1u << SCK); } while (0)
#define EPD_SCK_Set()  do { GPIO.out_w1ts = (1u << SCK); } while (0)
#define EPD_MOSI_Clr() do { GPIO.out_w1tc = (1u << MOSI); } while (0)
#define EPD_MOSI_Set() do { GPIO.out_w1ts = (1u << MOSI); } while (0)
#define EPD_RES_Clr()  do { GPIO.out1_w1tc.val = (1u << (RES - 32)); } while (0)
#define EPD_RES_Set()  do { GPIO.out1_w1ts.val = (1u << (RES - 32)); } while (0)
#define EPD_DC_Clr()   do { GPIO.out1_w1tc.val = (1u << (DC - 32)); } while (0)
#define EPD_DC_Set()   do { GPIO.out1_w1ts.val = (1u << (DC - 32)); } while (0)
#define EPD_CS_Clr()   do { GPIO.out1_w1tc.val = (1u << (CS - 32)); } while (0)
#define EPD_CS_Set()   do { GPIO.out1_w1ts.val = (1u << (CS - 32)); } while (0)
#define EPD_ReadBUSY   ((GPIO.in1.val >> (BUSY - 32)) & 1u)

enum EpdSpiPath : uint8_t {
  EPD_SPI_BITBANG = 0,
  EPD_SPI_HW = 1,
};

void EPD_GPIOInit(void);
void EPD_SetSpiPath(EpdSpiPath path);
EpdSpiPath EPD_GetSpiPath(void);
void EPD_SetHwSpiHz(uint32_t hz);
uint32_t EPD_GetHwSpiHz(void);
const char *EPD_SpiPathName(void);

void EPD_WR_Bus(uint8_t dat);
void EPD_WR_REG(uint8_t reg);
void EPD_WR_DATA8(uint8_t dat);
// Command + parameters in two transactions rather than one per byte.
void EPD_WR_CmdData(uint8_t cmd, const uint8_t *data, uint8_t n);

// Hold CS low across many bytes (HW path uses one SPI transaction)
void EPD_SpiBurstBegin(void);
void EPD_SpiBurstEnd(void);
void EPD_WR_DATA8_Burst(uint8_t dat);  // CS already low
// Bulk strided copy into the DMA buffer — `count` bytes taken `stride` apart.
// mask may be NULL; when given it is XORed per byte at the same stride.
// Avoids ~110 cycles of per-byte call overhead versus EPD_WR_DATA8_Burst.
void EPD_SpiBurstStrided(const uint8_t *src, const uint8_t *mask, uint32_t count,
                         uint32_t stride, uint8_t xconst);

#endif
