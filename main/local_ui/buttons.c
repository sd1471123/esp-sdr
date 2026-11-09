/* Five active-low keys with 20 ms debounce, auto-repeat, a 5 s long press on
 * the rotation key, and a LEFT+RIGHT mute chord. */
#include "buttons.h"
#include "board_c5_lcd.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include <string.h>

#define DEBOUNCE_US      20000
#define REPEAT_DELAY_US  400000
#define REPEAT_PERIOD_US 200000
#define LONG_PRESS_US    5000000
#define CHORD_WINDOW_US  350000

typedef struct {
    int pin;
    bool stable;
    bool raw;
    int64_t raw_change_us;
    int64_t pressed_us;
    int64_t last_repeat_us;
    bool long_sent;
    bool suppressed;    /* consumed by the mute chord */
    bool skip_short;    /* used as a modifier for a direction key */
    bool repeated;      /* auto-repeat already emitted for this press */
} button_state_t;

static button_state_t buttons[UI_BTN_COUNT] = {
    [UI_BTN_LEFT] = {.pin = BTN_PIN_LEFT},
    [UI_BTN_MODE] = {.pin = BTN_PIN_MODE},
    [UI_BTN_BAND] = {.pin = BTN_PIN_BAND},
    [UI_BTN_ROT] = {.pin = BTN_PIN_ROT},
    [UI_BTN_RIGHT] = {.pin = BTN_PIN_RIGHT},
};

void buttons_init(void) {
    for (unsigned j = 0; j < UI_BTN_COUNT; j++) {
        const gpio_config_t config = {
            .pin_bit_mask = UI_PIN_MASK(buttons[j].pin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&config);
        buttons[j].stable = buttons[j].raw = false;
        buttons[j].raw_change_us = esp_timer_get_time();
    }
}

bool buttons_down(ui_button_t button) { return buttons[button].stable; }

static void press_edge(unsigned j, int64_t now, ui_button_event_t *event) {
    buttons[j].pressed_us = now;
    buttons[j].last_repeat_us = now;
    buttons[j].long_sent = false;
    buttons[j].suppressed = false;
    buttons[j].repeated = false;
    if (j == UI_BTN_LEFT || j == UI_BTN_RIGHT) {
        /* BAND/MODE become combo modifiers while a direction key is held. */
        if (buttons[UI_BTN_BAND].stable) buttons[UI_BTN_BAND].skip_short = true;
        if (buttons[UI_BTN_MODE].stable) buttons[UI_BTN_MODE].skip_short = true;
        unsigned other = j == UI_BTN_LEFT ? UI_BTN_RIGHT : UI_BTN_LEFT;
        if (buttons[other].stable && !buttons[other].suppressed &&
            now - buttons[other].pressed_us <= CHORD_WINDOW_US) {
            buttons[other].suppressed = true;
            buttons[j].suppressed = true;
            event->chord_mute = true;
        }
    }
}

static void release_edge(unsigned j, ui_button_event_t *event) {
    if (!buttons[j].suppressed && !buttons[j].long_sent && !buttons[j].skip_short && !buttons[j].repeated)
        event->short_press |= (uint16_t)(1u << j);
    buttons[j].suppressed = false;
    buttons[j].skip_short = false;
    buttons[j].repeated = false;
}

void buttons_poll(ui_button_event_t *event) {
    memset(event, 0, sizeof(*event));
    int64_t now = esp_timer_get_time();
    for (unsigned j = 0; j < UI_BTN_COUNT; j++) {
        button_state_t *b = &buttons[j];
        bool raw = gpio_get_level(b->pin) == 0;
        if (raw != b->raw) {
            b->raw = raw;
            b->raw_change_us = now;
        }
        if (now - b->raw_change_us >= DEBOUNCE_US && b->stable != raw) {
            b->stable = raw;
            if (raw)
                press_edge(j, now, event);
            else
                release_edge(j, event);
        }
        if (!b->stable || b->suppressed) continue;
        if (j == UI_BTN_ROT && !b->long_sent && now - b->pressed_us >= LONG_PRESS_US) {
            b->long_sent = true;
            event->long_press |= (uint16_t)(1u << j);
        }
        if ((j == UI_BTN_LEFT || j == UI_BTN_RIGHT) && !b->skip_short &&
            now - b->pressed_us >= REPEAT_DELAY_US && now - b->last_repeat_us >= REPEAT_PERIOD_US) {
            b->last_repeat_us = now;
            b->repeated = true;
            event->repeat |= (uint16_t)(1u << j);
        }
    }
}
