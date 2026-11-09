/* Minimal polling SPI driver for the ST7789 panel. A small staging buffer is
 * used to byte-swap RGB565 pixels; it lives on the heap so the C5 SRAM window
 * reserved for the RF dump stays clear of static data. */
#include "st7789.h"
#include "board_c5_lcd.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include <string.h>

#define LCD_SPI_HOST SPI2_HOST
#define LCD_SPI_HZ   40000000
#define STAGE_PIXELS 512

static spi_device_handle_t lcd_spi;
static uint16_t *stage;
static unsigned lcd_w = 320, lcd_h = 240;
static bool landscape = true;

static void lcd_cmd(uint8_t cmd, const uint8_t *data, size_t len) {
    gpio_set_level(LCD_PIN_DC, 0);
    spi_transaction_t t = {.length = 8, .tx_buffer = &cmd};
    spi_device_polling_transmit(lcd_spi, &t);
    if (len) {
        gpio_set_level(LCD_PIN_DC, 1);
        spi_transaction_t d = {.length = len * 8, .tx_buffer = data};
        spi_device_polling_transmit(lcd_spi, &d);
    }
}

static void lcd_cmd1(uint8_t cmd, uint8_t value) { lcd_cmd(cmd, &value, 1); }

static void lcd_reset(void) {
    gpio_set_level(LCD_PIN_RST, 0);
    esp_rom_delay_us(20000);
    gpio_set_level(LCD_PIN_RST, 1);
    esp_rom_delay_us(120000);
}

bool lcd_init(void) {
    if (stage) return true;
    stage = heap_caps_malloc(STAGE_PIXELS * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (!stage) return false;

    const gpio_config_t out = {
        .pin_bit_mask = UI_PIN_MASK(LCD_PIN_DC) | UI_PIN_MASK(LCD_PIN_RST) | UI_PIN_MASK(LCD_PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out);
    gpio_set_level(LCD_PIN_BL, 0);
    gpio_set_level(LCD_PIN_DC, 1);
    gpio_set_level(LCD_PIN_RST, 1);

    spi_bus_config_t bus = {
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = LCD_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = STAGE_PIXELS * 2,
    };
    if (spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_DISABLED) != ESP_OK) return false;

    spi_device_interface_config_t dev = {
        .clock_speed_hz = LCD_SPI_HZ,
        .mode = 0,
        .spics_io_num = LCD_PIN_CS,
        .queue_size = 1,
    };
    if (spi_bus_add_device(LCD_SPI_HOST, &dev, &lcd_spi) != ESP_OK) return false;

    lcd_reset();
    lcd_cmd1(0x01, 0x00); /* software reset */
    esp_rom_delay_us(150000);
    lcd_cmd1(0x11, 0x00); /* sleep out */
    esp_rom_delay_us(120000);
    lcd_cmd1(0x3a, 0x55); /* 16-bit RGB565 */
    lcd_cmd1(0x36, landscape ? 0x60 : 0x00);
    const uint8_t porctrl[] = {0x0c, 0x0c, 0x00, 0x33, 0x33};
    lcd_cmd(0xb2, porctrl, sizeof(porctrl));
    lcd_cmd1(0xb7, 0x35);
    lcd_cmd1(0xbb, 0x1f);
    lcd_cmd1(0xc0, 0x2c);
    lcd_cmd1(0xc2, 0x01);
    lcd_cmd1(0xc3, 0x13);
    lcd_cmd1(0xc4, 0x20);
    lcd_cmd1(0xc6, 0x0f);
    const uint8_t pwctrl[] = {0xa4, 0xa1};
    lcd_cmd(0xd0, pwctrl, sizeof(pwctrl));
    const uint8_t gamma_pos[] = {0xd0, 0x08, 0x11, 0x08, 0x0c, 0x15, 0x39, 0x33,
                                 0x50, 0x36, 0x13, 0x14, 0x29, 0x2d};
    lcd_cmd(0xe0, gamma_pos, sizeof(gamma_pos));
    const uint8_t gamma_neg[] = {0xd0, 0x08, 0x10, 0x08, 0x06, 0x06, 0x39, 0x44,
                                 0x51, 0x0b, 0x16, 0x14, 0x2f, 0x31};
    lcd_cmd(0xe1, gamma_neg, sizeof(gamma_neg));
    lcd_cmd1(0x21, 0x00); /* inversion on, as required by this panel */
    lcd_cmd1(0x13, 0x00); /* normal display */
    lcd_cmd1(0x29, 0x00); /* display on */
    esp_rom_delay_us(20000);
    gpio_set_level(LCD_PIN_BL, 1);
    return true;
}

void lcd_set_rotation(bool want_landscape) {
    if (!lcd_spi || want_landscape == landscape) return;
    landscape = want_landscape;
    lcd_cmd1(0x36, landscape ? 0x60 : 0x00);
    if (landscape) {
        lcd_w = 320;
        lcd_h = 240;
    } else {
        lcd_w = 240;
        lcd_h = 320;
    }
}

unsigned lcd_width(void) { return lcd_w; }
unsigned lcd_height(void) { return lcd_h; }

uint16_t lcd_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

void lcd_set_window(unsigned x, unsigned y, unsigned w, unsigned h) {
    if (!w || !h) return;
    unsigned x1 = x + w - 1, y1 = y + h - 1;
    uint8_t cols[] = {x >> 8, x, x1 >> 8, x1};
    uint8_t rows[] = {y >> 8, y, y1 >> 8, y1};
    lcd_cmd(0x2a, cols, 4);
    lcd_cmd(0x2b, rows, 4);
    lcd_cmd(0x2c, NULL, 0);
}

void lcd_pixels(const uint16_t *pixels, unsigned count) {
    while (count) {
        unsigned chunk = count > STAGE_PIXELS ? STAGE_PIXELS : count;
        for (unsigned j = 0; j < chunk; j++) {
            uint16_t v = pixels[j];
            stage[j] = (uint16_t)((v >> 8) | (v << 8));
        }
        gpio_set_level(LCD_PIN_DC, 1);
        spi_transaction_t t = {.length = chunk * 16, .tx_buffer = stage};
        spi_device_polling_transmit(lcd_spi, &t);
        pixels += chunk;
        count -= chunk;
    }
}

void lcd_fill_rect(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color) {
    if (!w || !h) return;
    lcd_set_window(x, y, w, h);
    uint16_t swapped = (uint16_t)((color >> 8) | (color << 8));
    for (unsigned j = 0; j < STAGE_PIXELS; j++) stage[j] = swapped;
    unsigned total = w * h;
    gpio_set_level(LCD_PIN_DC, 1);
    while (total) {
        unsigned chunk = total > STAGE_PIXELS ? STAGE_PIXELS : total;
        spi_transaction_t t = {.length = chunk * 16, .tx_buffer = stage};
        spi_device_polling_transmit(lcd_spi, &t);
        total -= chunk;
    }
}

void lcd_fill(uint16_t color) { lcd_fill_rect(0, 0, lcd_w, lcd_h, color); }

void lcd_draw_bitmap2(unsigned x, unsigned y, unsigned w, unsigned h, const uint8_t *pixels,
                      const uint16_t palette[4]) {
    if (!w || !h || w > STAGE_PIXELS) return;
    lcd_set_window(x, y, w, h);
    gpio_set_level(LCD_PIN_DC, 1);
    for (unsigned row = 0; row < h; row++) {
        const uint8_t *src = pixels + row * ((w + 3) / 4);
        for (unsigned col = 0; col < w; col++) {
            unsigned shift = 6 - 2 * (col & 3);
            unsigned idx = (src[col >> 2] >> shift) & 3;
            stage[col] = (uint16_t)((palette[idx] >> 8) | (palette[idx] << 8));
        }
        spi_transaction_t t = {.length = w * 16, .tx_buffer = stage};
        spi_device_polling_transmit(lcd_spi, &t);
    }
}
