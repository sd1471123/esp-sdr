#include "burst_led.h"
#include "burst_serial.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_ESP_SDR_LED_PIN >= 0

#define LED_RESOLUTION_HZ 20000000u /* 50 ns per tick */
/* WS2812B timing in 50 ns ticks: bit period 25 ticks = 1.25 us. */
#define LED_T0H 8
#define LED_T0L 17
#define LED_T1H 16
#define LED_T1L 9

static const char *TAG = "burst_led";
static rmt_channel_handle_t tx_channel;
static rmt_encoder_handle_t copy_encoder;
static uint8_t current_r, current_g, current_b;

static void reply(const char *s) {
    burst_serial_send(s, strlen(s));
}

void burst_led_init(void) {
    const rmt_tx_channel_config_t channel_config = {
        .gpio_num = CONFIG_ESP_SDR_LED_PIN,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LED_RESOLUTION_HZ,
        .mem_block_symbols = 48,
        .trans_queue_depth = 2,
        .flags.invert_out = false,
        .flags.with_dma = false,
    };
    if (rmt_new_tx_channel(&channel_config, &tx_channel) != ESP_OK) {
        ESP_LOGW(TAG, "no RMT channel for the status LED");
        tx_channel = NULL;
        return;
    }
    const rmt_copy_encoder_config_t encoder_config; /* the struct is empty by design */
    if (rmt_new_copy_encoder(&encoder_config, &copy_encoder) != ESP_OK ||
        rmt_enable(tx_channel) != ESP_OK) {
        ESP_LOGW(TAG, "status LED encoder unavailable");
        tx_channel = NULL;
        return;
    }
    burst_led_set(0, 0, 0);
}

bool burst_led_set(uint8_t r, uint8_t g, uint8_t b) {
    if (tx_channel == NULL || copy_encoder == NULL) return false;
    /* WS2812 wants green first, then red, then blue, MSB first. */
    const uint8_t bytes[3] = {g, r, b};
    rmt_symbol_word_t symbols[24];
    unsigned n = 0;
    for (unsigned i = 0; i < 3; ++i) {
        for (int bit = 7; bit >= 0; --bit) {
            const bool one = (bytes[i] >> bit) & 1;
            symbols[n].level0 = 1;
            symbols[n].duration0 = one ? LED_T1H : LED_T0H;
            symbols[n].level1 = 0;
            symbols[n].duration1 = one ? LED_T1L : LED_T0L;
            ++n;
        }
    }
    const rmt_transmit_config_t transmit_config = {.loop_count = 0};
    if (rmt_transmit(tx_channel, copy_encoder, symbols, sizeof(symbols), &transmit_config) != ESP_OK)
        return false;
    if (rmt_tx_wait_all_done(tx_channel, 100) != ESP_OK) return false;
    current_r = r;
    current_g = g;
    current_b = b;
    /* The LED latches after the line has been low for >50 us; a tick is plenty. */
    vTaskDelay(1);
    return true;
}

int burst_led_owned_pin(void) {
    return CONFIG_ESP_SDR_LED_PIN;
}

bool burst_led_command(const char *line) {
    if (!strcmp(line, "LED?")) {
        char answer[32];
        snprintf(answer, sizeof(answer), "LED %u %u %u\n", current_r, current_g, current_b);
        reply(answer);
        return true;
    }
    if (strcmp(line, "LED") && strncmp(line, "LED ", 4)) return false;
    unsigned r, g, b;
    char extra;
    if (!strcmp(line, "LED 0")) {
        r = g = b = 0;
    } else if (sscanf(line, "LED %u %u %u %c", &r, &g, &b, &extra) != 3 || r > 255 || g > 255 ||
               b > 255) {
        reply("ERR led_args\n");
        return true;
    }
    char canonical[32];
    if (!strcmp(line, "LED 0")) {
        snprintf(canonical, sizeof(canonical), "LED 0");
    } else {
        snprintf(canonical, sizeof(canonical), "LED %u %u %u", r, g, b);
    }
    if (strcmp(line, canonical)) {
        reply("ERR led_args\n");
        return true;
    }
    if (!burst_led_set((uint8_t)r, (uint8_t)g, (uint8_t)b)) {
        reply("ERR led_io\n");
        return true;
    }
    reply("OK LED\n");
    return true;
}

#else /* no LED configured for this board */

void burst_led_init(void) {}

int burst_led_owned_pin(void) {
    return -1;
}

bool burst_led_set(uint8_t r, uint8_t g, uint8_t b) {
    (void)r;
    (void)g;
    (void)b;
    return false;
}

bool burst_led_command(const char *line) {
    (void)line;
    return false;
}

#endif
