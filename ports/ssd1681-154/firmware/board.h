#ifndef _BOARD_H_
#define _BOARD_H_

// Seeed XIAO ESP32-S3 + Seeed ePaper breakout, Watchy 1.54" glass.
// Panel: Good Display GDEH0154D67 (HINK-E154A07-A1), 200x200 mono, SSD1681.
// Same Xiao silkscreen pins as the 4.26". FPC 24-pin is NOT verified against
// Seeed's connector — do not plug the liberated glass until that is compared.

#include <Arduino.h>
#include "soc/gpio_struct.h"

#define EPD_RST 1
#define EPD_CS 2
#define EPD_BUSY 3
#define EPD_DC 4
#define EPD_SCK 7
#define EPD_MOSI 9

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

#define EPD_W 200
#define EPD_H 200
#define EPD_WBYTES (EPD_W / 8)                          // 25
#define EPD_PLANE_BYTES ((uint32_t)EPD_WBYTES * EPD_H)  // 5000

#define WHITE 0xFF
#define BLACK 0x00

#endif
