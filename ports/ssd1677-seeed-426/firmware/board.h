#ifndef _BOARD_H_
#define _BOARD_H_

// Seeed XIAO ESP32-S3 + Seeed ePaper Breakout/Driver Board v2
// Panel: GDEQ0426T82, 4.26" 800x480 mono, controller SSD1677.
//
// Pin map from the Seeed wiki (XIAO silkscreen -> ESP32-S3 GPIO):
//   RST  D0  -> GPIO1     BUSY D2  -> GPIO3     SCK  D8  -> GPIO7
//   CS   D1  -> GPIO2     DC   D3  -> GPIO4     MOSI D10 -> GPIO9
//
// Note: SPI2's IOMUX pins on the S3 are 11/12/13, so these route through the
// GPIO matrix. That is fine at the 20 MHz the panel allows, but do not expect
// to overclock past it — and the datasheet caps writes at 20 MHz anyway.

#include <Arduino.h>
#include "soc/gpio_struct.h"

#define EPD_RST 1
#define EPD_CS 2
#define EPD_BUSY 3
#define EPD_DC 4
#define EPD_SCK 7
#define EPD_MOSI 9

// All pins are below GPIO32, so the low banks serve every one of them (the
// 5.79 board needed the out1_* registers for its 45..48 pins; this one does not).
#define EPD_SCK_Clr() do { GPIO.out_w1tc = (1u << EPD_SCK); } while (0)
#define EPD_SCK_Set() do { GPIO.out_w1ts = (1u << EPD_SCK); } while (0)
#define EPD_MOSI_Clr() do { GPIO.out_w1tc = (1u << EPD_MOSI); } while (0)
#define EPD_MOSI_Set() do { GPIO.out_w1ts = (1u << EPD_MOSI); } while (0)
#define EPD_RES_Clr() do { GPIO.out_w1tc = (1u << EPD_RST); } while (0)
#define EPD_RES_Set() do { GPIO.out_w1ts = (1u << EPD_RST); } while (0)
#define EPD_DC_Clr() do { GPIO.out_w1tc = (1u << EPD_DC); } while (0)
#define EPD_DC_Set() do { GPIO.out_w1ts = (1u << EPD_DC); } while (0)
#define EPD_CS_Clr() do { GPIO.out_w1tc = (1u << EPD_CS); } while (0)
#define EPD_CS_Set() do { GPIO.out_w1ts = (1u << EPD_CS); } while (0)
#define EPD_ReadBUSY ((GPIO.in >> EPD_BUSY) & 1u)

// Panel geometry
#define EPD_W 800
#define EPD_H 480
#define EPD_WBYTES (EPD_W / 8)                        // 100
#define EPD_PLANE_BYTES ((uint32_t)EPD_WBYTES * EPD_H)  // 48000

#define WHITE 0xFF
#define BLACK 0x00

#endif
