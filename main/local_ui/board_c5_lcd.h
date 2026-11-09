/* ESP32-C5-DevKitC-1 with an 8-pin ST7789 320x240 SPI panel and five keys. */
#pragma once
#include <stdint.h>

/* LCD (SPI2_HOST). The panel header is GND VCC SCL SDI RESET AO CS BL,
 * where AO is the A0/DC line and SDI is MOSI (write-only, no MISO). */
#define LCD_PIN_SCK   6
#define LCD_PIN_MOSI  7
#define LCD_PIN_CS    10
#define LCD_PIN_DC    4
#define LCD_PIN_RST   5
#define LCD_PIN_BL    23

/* Keys, active low (internal pull-up). */
#define BTN_PIN_LEFT  0
#define BTN_PIN_RIGHT 1
#define BTN_PIN_BAND  8
#define BTN_PIN_MODE  9
#define BTN_PIN_ROT   24

/* Buzzer driven as a plain GPIO (no LEDC tone needed). */
#define BUZZER_PIN    25

#define UI_PIN_MASK(n) (UINT64_C(1) << (n))

/* Pins owned by the local UI; the serial GPIO command must never touch them. */
#define UI_RESERVED_PINS                                                                              \
    (UI_PIN_MASK(BUZZER_PIN) | UI_PIN_MASK(BTN_PIN_LEFT) | UI_PIN_MASK(BTN_PIN_MODE) |                \
     UI_PIN_MASK(BTN_PIN_BAND) | UI_PIN_MASK(BTN_PIN_ROT) | UI_PIN_MASK(BTN_PIN_RIGHT) |              \
     UI_PIN_MASK(LCD_PIN_SCK) | UI_PIN_MASK(LCD_PIN_MOSI) | UI_PIN_MASK(LCD_PIN_CS) |                 \
     UI_PIN_MASK(LCD_PIN_DC) | UI_PIN_MASK(LCD_PIN_RST) | UI_PIN_MASK(LCD_PIN_BL))
