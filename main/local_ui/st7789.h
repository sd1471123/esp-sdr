#pragma once
#include <stdbool.h>
#include <stdint.h>

/* ST7789 320x240 panel on SPI2_HOST. Coordinates are top-left based. */
bool lcd_init(void);
void lcd_set_rotation(bool landscape);
unsigned lcd_width(void);
unsigned lcd_height(void);
uint16_t lcd_rgb565(uint8_t r, uint8_t g, uint8_t b);

void lcd_fill(uint16_t color);
void lcd_fill_rect(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color);
void lcd_set_window(unsigned x, unsigned y, unsigned w, unsigned h);
void lcd_pixels(const uint16_t *pixels, unsigned count);
/* 2 bits per pixel, most significant bits first; 4-entry RGB565 palette. */
void lcd_draw_bitmap2(unsigned x, unsigned y, unsigned w, unsigned h, const uint8_t *pixels,
                      const uint16_t palette[4]);
