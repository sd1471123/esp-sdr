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
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

/* Header pins only. 15-22 are the in-package memory pads, 11/12 stay with the
 * console, and 25 is the buzzer we talk through. 13/14 are USB D-/D+: while a
 * cable is plugged in the host holds D- low, so they always look "stuck" and
 * grounding them fights the PHY. They are the two pins the panel wiring should
 * never use anyway. */
static const int probe_pins[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 23, 24, 26, 27, 28};
#define PROBE_COUNT (sizeof(probe_pins) / sizeof(probe_pins[0]))

/* Text goes to stdout, which the project points at whichever console it was
 * built with: UART0 on the probe image, USB-Serial-JTAG on the panel image.
 * printf is used rather than a private driver because installing the
 * usb_serial_jtag driver while the console already owns that peripheral
 * deadlocks the task before a single byte leaves the chip. */
static void say(const char *text) {
    esp_rom_printf("%s", text);
}

static void say_pin(int pin, const char *suffix) {
    char line[32];
    if (snprintf(line, sizeof(line), "PIN %d %s\n", pin, suffix) > 0) say(line);
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
    say("PROBE boot\n");

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

    say("PROBE ready, touch a header pin to GND\n");

    rest(400);
    alive();

    /* One line with every pin and its level: 'L' means something is holding it
     * down before any key was pressed. */
    say("PROBE levels:");
    for (size_t j = 0; j < PROBE_COUNT; j++) {
        char item[10];
        snprintf(item, sizeof(item), " %d%c", probe_pins[j],
                 gpio_get_level(probe_pins[j]) ? 'H' : 'L');
        say(item);
    }
    say("\n");

    /* Anything already low at boot is shorted, not pressed: report it once, in
     * text and in beeps, then leave it out of the loop. A permanently grounded
     * pin would otherwise sit in the release wait below and mute the probe. */
    bool stuck[PROBE_COUNT] = {false};
    size_t stuck_count = 0;
    for (size_t j = 0; j < PROBE_COUNT; j++) {
        if (gpio_get_level(probe_pins[j]) != 0) continue;
        stuck[j] = true;
        stuck_count++;
        say_pin(probe_pins[j], "low at boot");
    }
    if (stuck_count) {
        say("PROBE stuck low at boot\n");
        for (size_t j = 0; j < PROBE_COUNT; j++)
            if (stuck[j]) announce(probe_pins[j]);
        say("PROBE listening, the pins above are ignored\n");
    }

    for (;;) {
        for (size_t j = 0; j < PROBE_COUNT; j++) {
            if (stuck[j]) continue;
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
