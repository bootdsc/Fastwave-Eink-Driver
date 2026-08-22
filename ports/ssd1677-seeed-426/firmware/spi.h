#ifndef _SPI_H_
#define _SPI_H_


#include "board.h"

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
