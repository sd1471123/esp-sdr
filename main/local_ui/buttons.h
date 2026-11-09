#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    UI_BTN_LEFT = 0,
    UI_BTN_MODE,
    UI_BTN_BAND,
    UI_BTN_ROT,
    UI_BTN_RIGHT,
    UI_BTN_COUNT
} ui_button_t;

typedef struct {
    uint16_t short_press; /* one bit per ui_button_t */
    uint16_t repeat;      /* direction keys, held down */
    uint16_t long_press;  /* rotation key only */
    bool chord_mute;      /* LEFT+RIGHT within 350 ms */
    uint8_t dir_mod;      /* modifier captured at the direction press: 1 top, 2 bottom */
} ui_button_event_t;

void buttons_init(void);
void buttons_poll(ui_button_event_t *event);
bool buttons_down(ui_button_t button);
