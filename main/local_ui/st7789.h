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
/* Multi-call pixel stream: lcd_set_window() once, lcd_pixels_cont() per row,
 * then lcd_pixels_end(). CS stays low for the whole window. */
void lcd_pixels_cont(const uint16_t *pixels, unsigned count);
void lcd_pixels_end(void);
/* 2 bits per pixel, most significant bits first; 4-entry RGB565 palette. */
void lcd_draw_bitmap2(unsigned x, unsigned y, unsigned w, unsigned h, const uint8_t *pixels,
                      const uint16_t palette[4]);
/* Backlight brightness, 0-100 %. GP23 is a PWM channel after lcd_init(), so
 * this is a real dimmer and not just an on/off switch. 0 turns the panel dark
 * without touching the display controller, so the picture is still there when
 * the brightness comes back. */
void lcd_backlight_set(uint8_t pct);
