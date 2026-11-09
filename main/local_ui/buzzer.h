#pragma once
#include <stdint.h>

typedef enum {
    UI_BUZZ_BOOT = 0,
    UI_BUZZ_BAND,
    UI_BUZZ_MODE,
    UI_BUZZ_CHANNEL,
    UI_BUZZ_TOP,
    UI_BUZZ_BOTTOM,
    UI_BUZZ_COUNTDOWN,
    UI_BUZZ_RESET,
    UI_BUZZ_COUNT
} ui_buzz_t;

void buzzer_init(void);
void buzzer_play(ui_buzz_t sound);
void buzzer_enable(bool enabled);
