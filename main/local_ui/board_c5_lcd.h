/* Waveshare-style 3.2" ST7789 320x240 SPI panel and five front-panel keys. */
#pragma once
#include <stdint.h>

/* LCD (SPI2_HOST) */
#define LCD_PIN_SCK   6
#define LCD_PIN_MOSI  7
#define LCD_PIN_CS    8
#define LCD_PIN_DC    9
#define LCD_PIN_RST   10
#define LCD_PIN_BL    23

/* Keys, active low (internal pull-up). */
#define BTN_PIN_LEFT  2
#define BTN_PIN_MODE  3
#define BTN_PIN_BAND  4
#define BTN_PIN_ROT   5
#define BTN_PIN_RIGHT 24

/* Buzzer driven as a plain GPIO (no LEDC tone needed). */
#define BUZZER_PIN    1

#define UI_PIN_MASK(n) (UINT64_C(1) << (n))

/* Pins owned by the local UI; the serial GPIO command must never touch them. */
#define UI_RESERVED_PINS                                                                              \
    (UI_PIN_MASK(BUZZER_PIN) | UI_PIN_MASK(BTN_PIN_LEFT) | UI_PIN_MASK(BTN_PIN_MODE) |                \
     UI_PIN_MASK(BTN_PIN_BAND) | UI_PIN_MASK(BTN_PIN_ROT) | UI_PIN_MASK(BTN_PIN_RIGHT) |              \
     UI_PIN_MASK(LCD_PIN_SCK) | UI_PIN_MASK(LCD_PIN_MOSI) | UI_PIN_MASK(LCD_PIN_CS) |                 \
     UI_PIN_MASK(LCD_PIN_DC) | UI_PIN_MASK(LCD_PIN_RST) | UI_PIN_MASK(LCD_PIN_BL))
