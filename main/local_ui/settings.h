#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { UI_BAND_24G = 0, UI_BAND_5G = 1 } ui_band_t;
typedef enum { UI_MODE_SWEEP = 0, UI_MODE_LIVE = 1 } ui_mode_t;

typedef struct {
    uint8_t band;      /* ui_band_t */
    uint8_t mode;      /* ui_mode_t */
    uint8_t landscape; /* 1 = 320x240 */
    uint8_t buzz;      /* 1 = buzzer enabled */
    uint8_t mute;      /* 1 = muted (persisted with the chord) */
    int8_t top_db;         /* SWEEP upper limit */
    int8_t bottom_db;      /* SWEEP lower limit */
    int8_t live_top_db;    /* LIVE upper limit — independent of SWEEP */
    int8_t live_bottom_db; /* LIVE lower limit */
    /* Both modes are fixed; there is no auto-range flag any more. */
} ui_settings_t;

void ui_settings_defaults(ui_settings_t *settings);
void ui_settings_load(ui_settings_t *settings);
void ui_settings_save(const ui_settings_t *settings);
void ui_settings_clear(void);
