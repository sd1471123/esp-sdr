/* Diagnostic build: skip the radio and the panel entirely and turn the header
 * into a keypad. Every broken-out GPIO is an input with a pull-up; grounding
 * one with a jumper makes the buzzer count its number, so a mis-silkscreened
 * board or a swapped lead answers itself in seconds.
 *
 * Digits are announced as groups of 160 ms notes:
 *
 *   400 ms header  ->  tens group  ->  600 ms rest  ->  ones group
 *
 * So GPIO24 is 2 short notes, a pause, then 4; GPIO6 is 6 notes with no pause;
 * a trailing long 700 ms note means 0. Nothing here touches the LCD, so this
 * build works even when the panel stays black. */
#include "pin_probe.h"

#if CONFIG_ESP_SDR_PIN_PROBE

#include "board_c5_lcd.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

/* Header pins only. 15-22 are the in-package memory pads, 11/12 stay with the
 * console, and 25 is the buzzer we talk through. */
static const int probe_pins[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 13, 14, 23, 24, 26, 27, 28};
#define PROBE_COUNT (sizeof(probe_pins) / sizeof(probe_pins[0]))

static bool console_ready;

static void say(const char *text) {
    if (console_ready) usb_serial_jtag_write_bytes(text, strlen(text), pdMS_TO_TICKS(20));
}

static void say_pin(int pin, const char *suffix) {
    char line[32];
    int n = snprintf(line, sizeof(line), "PIN %d %s\n", pin, suffix);
    if (console_ready && n > 0) usb_serial_jtag_write_bytes(line, (size_t)n, pdMS_TO_TICKS(20));
}

static void note(int ms) {
    gpio_set_level(BUZZER_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(ms));
    gpio_set_level(BUZZER_PIN, 0);
}

static void rest(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

static void digit(int value) {
    for (int j = 0; j < value; j++) {
        note(160);
        rest(160);
    }
}

static void announce(int pin) {
    note(400); /* header, so a number never starts without a cue */
    rest(400);
    int tens = pin / 10, ones = pin % 10;
    if (tens) {
        digit(tens);
        rest(600);
    }
    if (ones == 0)
        note(700); /* a long note is a zero */
    else
        digit(ones);
    rest(900);
}

static void alive(void) {
    note(60);
    rest(90);
    note(60);
    rest(90);
    note(60);
    rest(700);
}

void pin_probe_run(void) {
    const gpio_config_t out = {
        .pin_bit_mask = UI_PIN_MASK(BUZZER_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out);
    gpio_set_level(BUZZER_PIN, 0);

    for (size_t j = 0; j < PROBE_COUNT; j++) {
        const gpio_config_t in = {
            .pin_bit_mask = UI_PIN_MASK(probe_pins[j]),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&in);
    }

    usb_serial_jtag_driver_config_t usb = {.tx_buffer_size = 256, .rx_buffer_size = 256};
    console_ready = usb_serial_jtag_driver_install(&usb) == ESP_OK;
    say("PROBE ready, touch a header pin to GND\n");

    rest(400);
    alive();

    /* Anything already low at boot is shorted, not pressed: report it first. */
    int stuck[PROBE_COUNT];
    size_t stuck_count = 0;
    for (size_t j = 0; j < PROBE_COUNT; j++)
        if (gpio_get_level(probe_pins[j]) == 0) stuck[stuck_count++] = probe_pins[j];
    if (stuck_count) {
        say("PROBE stuck low at boot\n");
        for (size_t j = 0; j < stuck_count; j++) announce(stuck[j]);
    }

    for (;;) {
        for (size_t j = 0; j < PROBE_COUNT; j++) {
            int pin = probe_pins[j];
            if (gpio_get_level(pin) != 0) continue;
            rest(25); /* debounce */
            if (gpio_get_level(pin) != 0) continue;
            say_pin(pin, "low");
            announce(pin);
            while (gpio_get_level(pin) == 0) rest(20); /* wait for release */
            rest(250);
        }
        rest(10);
    }
}

#endif
