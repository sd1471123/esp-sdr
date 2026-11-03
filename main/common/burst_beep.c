#include "burst_beep.h"
#include "burst_serial.h"
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"

#define BEEP_RESOLUTION_HZ 1000000u /* 1 us per tick */
#define BEEP_MIN_HZ 100
#define BEEP_MAX_HZ 20000
#define BEEP_MAX_PIN 48

static const char *TAG = "burst_beep";
static rmt_channel_handle_t beep_channel;
static rmt_encoder_handle_t beep_encoder;
static int beep_pin = -1;
static unsigned beep_freq;
static unsigned beep_duty;

static void reply(const char *s) {
    burst_serial_send(s, strlen(s));
}

static void beep_stop(void) {
    if (beep_channel != NULL) {
        rmt_disable(beep_channel);
        rmt_del_channel(beep_channel);
        beep_channel = NULL;
    }
    if (beep_pin >= 0) gpio_reset_pin(beep_pin);
    beep_pin = -1;
    beep_freq = 0;
    beep_duty = 0;
}

void burst_beep_init(void) {
    const rmt_copy_encoder_config_t encoder_config; /* empty by design */
    if (rmt_new_copy_encoder(&encoder_config, &beep_encoder) != ESP_OK) {
        ESP_LOGW(TAG, "no encoder for the buzzer");
        beep_encoder = NULL;
    }
    beep_pin = -1;
    beep_freq = 0;
    beep_duty = 0;
}

/* One symbol carries both phases of a period and is looped forever by the RMT
 * hardware, so the tone keeps playing without any CPU time. */
static bool beep_start(int pin, unsigned freq, unsigned duty) {
    if (beep_encoder == NULL) return false;
    if (beep_channel != NULL && beep_pin != pin) beep_stop();
    if (beep_channel == NULL) {
        const rmt_tx_channel_config_t channel_config = {
            .gpio_num = pin,
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = BEEP_RESOLUTION_HZ,
            .mem_block_symbols = 48,
            .trans_queue_depth = 2,
            .flags.invert_out = false,
            .flags.with_dma = false,
        };
        if (rmt_new_tx_channel(&channel_config, &beep_channel) != ESP_OK) {
            ESP_LOGW(TAG, "no RMT channel for the buzzer");
            beep_channel = NULL;
            return false;
        }
    } else {
        rmt_disable(beep_channel);
    }
    const unsigned period = BEEP_RESOLUTION_HZ / freq;
    if (period < 2) return false;
    unsigned high = period * duty / 100;
    if (high < 1) high = 1;
    if (high >= period) high = period - 1;

    rmt_symbol_word_t symbol;
    symbol.level0 = 1;
    symbol.duration0 = high;
    symbol.level1 = 0;
    symbol.duration1 = period - high;

    if (rmt_enable(beep_channel) != ESP_OK) return false;
    const rmt_transmit_config_t transmit_config = {.loop_count = -1};
    if (rmt_transmit(beep_channel, beep_encoder, &symbol, sizeof(symbol), &transmit_config) != ESP_OK)
        return false;
    beep_pin = pin;
    beep_freq = freq;
    beep_duty = duty;
    return true;
}

int burst_beep_owned_pin(void) {
    return beep_duty > 0 ? beep_pin : -1;
}

void burst_beep_silence(void) {
    /* beep_stop() is idempotent: it only releases what it owns. */
    beep_stop();
}

bool burst_beep_command(const char *line) {
    if (!strcmp(line, "BEEP?")) {
        char answer[48];
        if (beep_duty > 0)
            snprintf(answer, sizeof(answer), "BEEP %d %u %u\n", beep_pin, beep_freq, beep_duty);
        else
            snprintf(answer, sizeof(answer), "BEEP 0\n");
        reply(answer);
        return true;
    }
    if (strcmp(line, "BEEP") && strncmp(line, "BEEP ", 5)) return false;
    if (!strcmp(line, "BEEP 0")) {
        beep_stop();
        reply("OK BEEP 0\n");
        return true;
    }
    unsigned pin, freq, duty;
    char extra;
    char canonical[40];
    if (sscanf(line, "BEEP %u %u %u %c", &pin, &freq, &duty, &extra) == 3) {
        snprintf(canonical, sizeof(canonical), "BEEP %u %u %u", pin, freq, duty);
        if (!strcmp(line, canonical) && pin <= BEEP_MAX_PIN && freq >= BEEP_MIN_HZ &&
            freq <= BEEP_MAX_HZ && duty <= 100) {
            if (duty == 0) {
                beep_stop();
                reply("OK BEEP 0\n");
                return true;
            }
            if (beep_start((int)pin, freq, duty)) {
                reply("OK BEEP\n");
            } else {
                reply("ERR beep_io\n");
            }
            return true;
        }
    }
    reply("ERR beep_args\n");
    return true;
}
