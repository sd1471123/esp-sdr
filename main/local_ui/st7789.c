/* Minimal polling SPI driver for the 3.2" 240x320 panel on the local UI. The
 * controller is an ILI9341 (GMT032-8P-SPI module); an ST7789 variant is kept
 * behind LCD_CTRL_ILI9341 for other panels. A small staging buffer is used to
 * byte-swap RGB565 pixels; it lives on the heap so the C5 SRAM window reserved
 * for the RF dump stays clear of static data. */
#include "st7789.h"
#include "board_c5_lcd.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include <string.h>

#define LCD_SPI_HOST SPI2_HOST
#define STAGE_PIXELS 512

/* Controller on the 8-pin GMT032-8P-SPI module is an ST7789 (240x320). An
 * ILI9341 power-on sequence is kept behind LCD_CTRL_ILI9341 because the two
 * parts look identical from the header and the wrong sequence shows up as
 * nothing but GRAM noise. */
#ifndef LCD_CTRL_ILI9341
#define LCD_CTRL_ILI9341 0
#endif

/* MADCTL per orientation. The panel is 240x320; landscape swaps the axes. */
#if LCD_CTRL_ILI9341
#define LCD_MADCTL_LAND 0x28 /* MV | BGR */
#define LCD_MADCTL_PORT 0x48 /* MX | BGR */
#else
#define LCD_MADCTL_LAND 0x60 /* MX | MV */
#define LCD_MADCTL_PORT 0x00
#endif

/* Bring-up escape hatches. The module header has two adjacent pairs that are
 * trivially swapped when hand-wiring: SCL/SDI and RES/AO. Flipping these two
 * macros rebuilds the driver against the other pin order, which is faster than
 * re-seating four dupont leads when the panel stays blank. */
#ifndef LCD_SWAP_SCL_SDI
#define LCD_SWAP_SCL_SDI 0
#endif
#ifndef LCD_SWAP_DC_RST
#define LCD_SWAP_DC_RST 0
#endif

#if LCD_SWAP_SCL_SDI
#define PIN_SCK  LCD_PIN_MOSI
#define PIN_MOSI LCD_PIN_SCK
#else
#define PIN_SCK  LCD_PIN_SCK
#define PIN_MOSI LCD_PIN_MOSI
#endif
#if LCD_SWAP_DC_RST
#define PIN_DC  LCD_PIN_RST
#define PIN_RST LCD_PIN_DC
#else
#define PIN_DC  LCD_PIN_DC
#define PIN_RST LCD_PIN_RST
#endif

/* Bring-up knobs. A panel on 15 cm dupont wires rarely survives 40 MHz: the
 * data lines ring and the controller latches shifted pixels, which looks like
 * random colour noise. 20 MHz is the safe default; raise it once the picture
 * is stable. */
#ifndef LCD_SPI_HZ
#define LCD_SPI_HZ 20000000
#endif

/* Visible-area offset inside the controller GRAM. A 240x320 panel needs 0/0,
 * 240x240 squares sit at y=80. If the image is shifted by a constant amount or
 * wrapped, change these two instead of touching the drawing code. */
#ifndef LCD_OFS_X
#define LCD_OFS_X 0
#endif
#ifndef LCD_OFS_Y
#define LCD_OFS_Y 0
#endif

/* Paint solid bars and a 1 px frame right after init so a wiring fault can be
 * told apart from a drawing fault. Set to 0 for a silent boot. */
#ifndef LCD_SELFTEST
#define LCD_SELFTEST 1
#endif

static spi_device_handle_t lcd_spi;
static uint16_t *stage;
static unsigned lcd_w = 320, lcd_h = 240;
static bool landscape = true;

static void lcd_selftest(void);

static void lcd_cmd(uint8_t cmd, const uint8_t *data, size_t len) {
    gpio_set_level(PIN_DC, 0);
    spi_transaction_t t = {.length = 8, .tx_buffer = &cmd};
    spi_device_polling_transmit(lcd_spi, &t);
    if (len) {
        gpio_set_level(PIN_DC, 1);
        spi_transaction_t d = {.length = len * 8, .tx_buffer = data};
        spi_device_polling_transmit(lcd_spi, &d);
    }
}

static void lcd_cmd1(uint8_t cmd, uint8_t value) { lcd_cmd(cmd, &value, 1); }

static void lcd_reset(void) {
    gpio_set_level(PIN_RST, 0);
    esp_rom_delay_us(20000);
    gpio_set_level(PIN_RST, 1);
    esp_rom_delay_us(120000);
}

bool lcd_init(void) {
    if (stage) return true;
    stage = heap_caps_malloc(STAGE_PIXELS * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (!stage) return false;

    const gpio_config_t out = {
        .pin_bit_mask = UI_PIN_MASK(PIN_DC) | UI_PIN_MASK(PIN_RST) | UI_PIN_MASK(LCD_PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out);
    gpio_set_level(LCD_PIN_BL, 0);
    gpio_set_level(PIN_DC, 1);
    gpio_set_level(PIN_RST, 1);

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_SCK,
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
#if LCD_CTRL_ILI9341
    lcd_cmd(0x01, NULL, 0); /* software reset */
    esp_rom_delay_us(10000);
    lcd_cmd(0x28, NULL, 0); /* display off while the power stages settle */

    const uint8_t pcb[] = {0x00, 0xc1, 0x30};
    lcd_cmd(0xcf, pcb, sizeof(pcb)); /* power control B */
    const uint8_t posc[] = {0x64, 0x03, 0x12, 0x81};
    lcd_cmd(0xed, posc, sizeof(posc)); /* power on sequence control */
    const uint8_t dta[] = {0x85, 0x00, 0x78};
    lcd_cmd(0xe8, dta, sizeof(dta)); /* driver timing control A */
    const uint8_t pca[] = {0x39, 0x2c, 0x00, 0x34, 0x02};
    lcd_cmd(0xcb, pca, sizeof(pca)); /* power control A */
    lcd_cmd1(0xf7, 0x20);            /* pump ratio control */
    const uint8_t dtb[] = {0x00, 0x00};
    lcd_cmd(0xea, dtb, sizeof(dtb));  /* driver timing control B */
    lcd_cmd1(0xc0, 0x23);             /* power control 1, GVDD 4.60 V */
    lcd_cmd1(0xc1, 0x10);             /* power control 2, step-up factor */
    const uint8_t vcom1[] = {0x3e, 0x28};
    lcd_cmd(0xc5, vcom1, sizeof(vcom1)); /* VCOM control 1 */
    lcd_cmd1(0xc7, 0x86);                /* VCOM control 2, offset -6 */
    lcd_cmd1(0x36, landscape ? LCD_MADCTL_LAND : LCD_MADCTL_PORT);
    lcd_cmd1(0x3a, 0x55); /* 16-bit RGB565 */

    const uint8_t frate[] = {0x00, 0x18};
    lcd_cmd(0xb1, frate, sizeof(frate)); /* frame rate 79 Hz */
    const uint8_t dfc[] = {0x08, 0x82, 0x27};
    lcd_cmd(0xb6, dfc, sizeof(dfc)); /* display function control */
    lcd_cmd1(0xf2, 0x00);            /* 3 gamma off */
    lcd_cmd1(0x26, 0x01);            /* gamma curve 1 */
    const uint8_t gamma_pos[] = {0x0f, 0x31, 0x2b, 0x0c, 0x0e, 0x08, 0x4e, 0xf1,
                                 0x37, 0x07, 0x10, 0x03, 0x0e, 0x09, 0x00};
    lcd_cmd(0xe0, gamma_pos, sizeof(gamma_pos));
    const uint8_t gamma_neg[] = {0x00, 0x0e, 0x14, 0x03, 0x11, 0x07, 0x31, 0xc1,
                                 0x48, 0x08, 0x0f, 0x0c, 0x31, 0x36, 0x0f};
    lcd_cmd(0xe1, gamma_neg, sizeof(gamma_neg));

    lcd_cmd(0x11, NULL, 0); /* sleep out */
    esp_rom_delay_us(120000);
    lcd_cmd(0x29, NULL, 0); /* display on */
    esp_rom_delay_us(20000);
#else
    lcd_cmd1(0x01, 0x00); /* software reset */
    esp_rom_delay_us(150000);
    lcd_cmd1(0x11, 0x00); /* sleep out */
    esp_rom_delay_us(120000);
    lcd_cmd1(0x3a, 0x55); /* 16-bit RGB565 */
    lcd_cmd1(0x36, landscape ? LCD_MADCTL_LAND : LCD_MADCTL_PORT);
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
    lcd_cmd1(0x21, 0x00); /* inversion on */
    lcd_cmd1(0x13, 0x00); /* normal display */
    lcd_cmd1(0x29, 0x00); /* display on */
    esp_rom_delay_us(20000);
#endif
    gpio_set_level(LCD_PIN_BL, 1);
#if LCD_SELFTEST
    lcd_selftest();
#endif
    return true;
}

void lcd_set_rotation(bool want_landscape) {
    if (!lcd_spi || want_landscape == landscape) return;
    landscape = want_landscape;
    lcd_cmd1(0x36, landscape ? LCD_MADCTL_LAND : LCD_MADCTL_PORT);
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
    x += LCD_OFS_X;
    y += LCD_OFS_Y;
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
        gpio_set_level(PIN_DC, 1);
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
    gpio_set_level(PIN_DC, 1);
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
    gpio_set_level(PIN_DC, 1);
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

#if LCD_SELFTEST
/* Solid bars first, then a 1 px frame with a checkerboard. If the frame hugs
 * all four edges the bus, the window and MADCTL agree with the panel; a torn
 * or wrapped frame points at the resolution offset, and speckle inside a solid
 * bar points at the SPI clock or a floating data line. */
static void lcd_selftest(void) {
    static const uint16_t bars[] = {0xf800, 0x07e0, 0x001f, 0xffff, 0x0000};
    for (unsigned i = 0; i < sizeof(bars) / sizeof(bars[0]); i++) {
        lcd_fill(bars[i]);
        esp_rom_delay_us(250000);
    }
    lcd_fill(0x0000);
    lcd_fill_rect(0, 0, lcd_w, 1, 0xffff);
    lcd_fill_rect(0, lcd_h - 1, lcd_w, 1, 0xffff);
    lcd_fill_rect(0, 0, 1, lcd_h, 0xffff);
    lcd_fill_rect(lcd_w - 1, 0, 1, lcd_h, 0xffff);
    for (unsigned y = 0; y < lcd_h; y += 16) {
        for (unsigned x = 0; x < lcd_w; x += 16) {
            if (((x >> 4) + (y >> 4)) & 1) lcd_fill_rect(x, y, 16, 16, 0xffff);
        }
    }
    esp_rom_delay_us(800000);
    lcd_fill(0x0000);
}
#endif
