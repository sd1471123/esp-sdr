/* Persisted front-panel state. NVS is already initialized by app_main. */
#include "settings.h"
#include "nvs.h"
#include <string.h>

#define UI_NVS_NAMESPACE "sdr_ui"

static int8_t clamp_db(int8_t v, int8_t lo, int8_t hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

void ui_settings_defaults(ui_settings_t *s) {
    s->band = UI_BAND_5G;
    s->mode = UI_MODE_SWEEP;
    s->landscape = 1;
    s->buzz = 1;
    s->mute = 0;
    s->bright = 100;
    for (unsigned b = 0; b < 2; b++) {
        s->top_db[b] = -26;
        s->bottom_db[b] = -105;
        s->live_top_db[b] = -26;
        s->live_bottom_db[b] = -105;
    }
}

static void sanitize(ui_settings_t *s) {
    if (s->band > UI_BAND_5G) s->band = UI_BAND_5G;
    if (s->mode > UI_MODE_LIVE) s->mode = UI_MODE_LIVE;
    if (s->landscape > 1) s->landscape = 1;
    if (s->buzz > 1) s->buzz = 1;
    if (s->mute > 1) s->mute = 1;
    /* Only the two presets the chord can reach are valid; anything else in NVS
     * (an old image, a bad write) snaps to the nearer one. A stored 0 from the
     * first PWM build lands on 10 % on purpose: booting into a black screen looks
     * exactly like a bricked board. */
    s->bright = s->bright >= 55 ? 100 : 10;
    for (unsigned b = 0; b < 2; b++) {
        s->top_db[b] = clamp_db(s->top_db[b], -80, -10);
        s->bottom_db[b] = clamp_db(s->bottom_db[b], -125, -55);
        if (s->bottom_db[b] > s->top_db[b] - 30) s->bottom_db[b] = s->top_db[b] - 30;
        s->live_top_db[b] = clamp_db(s->live_top_db[b], -80, -10);
        s->live_bottom_db[b] = clamp_db(s->live_bottom_db[b], -125, -55);
        if (s->live_bottom_db[b] > s->live_top_db[b] - 30)
            s->live_bottom_db[b] = s->live_top_db[b] - 30;
    }
}

void ui_settings_load(ui_settings_t *s) {
    ui_settings_defaults(s);
    nvs_handle_t handle;
    if (nvs_open(UI_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(handle, "band", &v) == ESP_OK) s->band = v;
    if (nvs_get_u8(handle, "mode", &v) == ESP_OK) s->mode = v;
    if (nvs_get_u8(handle, "land", &v) == ESP_OK) s->landscape = v;
    if (nvs_get_u8(handle, "buzz", &v) == ESP_OK) s->buzz = v;
    if (nvs_get_u8(handle, "mute", &v) == ESP_OK) s->mute = v;
    if (nvs_get_u8(handle, "bright", &v) == ESP_OK) s->bright = v;
    int8_t d;
    /* Legacy keys held one pair per mode with no band dimension. They become
     * the 5G pairs (the default band) so an upgrade keeps the current picture. */
    if (nvs_get_i8(handle, "top", &d) == ESP_OK) s->top_db[UI_BAND_5G] = d;
    if (nvs_get_i8(handle, "bot", &d) == ESP_OK) s->bottom_db[UI_BAND_5G] = d;
    if (nvs_get_i8(handle, "ltop", &d) == ESP_OK) s->live_top_db[UI_BAND_5G] = d;
    if (nvs_get_i8(handle, "lbot", &d) == ESP_OK) s->live_bottom_db[UI_BAND_5G] = d;
    /* 2.4G starts from whatever 5G has, then its own keys take precedence. */
    s->top_db[UI_BAND_24G] = s->top_db[UI_BAND_5G];
    s->bottom_db[UI_BAND_24G] = s->bottom_db[UI_BAND_5G];
    s->live_top_db[UI_BAND_24G] = s->live_top_db[UI_BAND_5G];
    s->live_bottom_db[UI_BAND_24G] = s->live_bottom_db[UI_BAND_5G];
    if (nvs_get_i8(handle, "top24", &d) == ESP_OK) s->top_db[UI_BAND_24G] = d;
    if (nvs_get_i8(handle, "bot24", &d) == ESP_OK) s->bottom_db[UI_BAND_24G] = d;
    if (nvs_get_i8(handle, "ltop24", &d) == ESP_OK) s->live_top_db[UI_BAND_24G] = d;
    if (nvs_get_i8(handle, "lbot24", &d) == ESP_OK) s->live_bottom_db[UI_BAND_24G] = d;
    nvs_close(handle);
    sanitize(s);
}

void ui_settings_save(const ui_settings_t *s) {
    nvs_handle_t handle;
    if (nvs_open(UI_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, "band", s->band);
    nvs_set_u8(handle, "mode", s->mode);
    nvs_set_u8(handle, "land", s->landscape);
    nvs_set_u8(handle, "buzz", s->buzz);
    nvs_set_u8(handle, "mute", s->mute);
    nvs_set_u8(handle, "bright", s->bright);
    nvs_set_i8(handle, "top", s->top_db[UI_BAND_5G]);
    nvs_set_i8(handle, "bot", s->bottom_db[UI_BAND_5G]);
    nvs_set_i8(handle, "ltop", s->live_top_db[UI_BAND_5G]);
    nvs_set_i8(handle, "lbot", s->live_bottom_db[UI_BAND_5G]);
    nvs_set_i8(handle, "top24", s->top_db[UI_BAND_24G]);
    nvs_set_i8(handle, "bot24", s->bottom_db[UI_BAND_24G]);
    nvs_set_i8(handle, "ltop24", s->live_top_db[UI_BAND_24G]);
    nvs_set_i8(handle, "lbot24", s->live_bottom_db[UI_BAND_24G]);
    nvs_commit(handle);
    nvs_close(handle);
}

void ui_settings_clear(void) {
    nvs_handle_t handle;
    if (nvs_open(UI_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_erase_all(handle);
    nvs_commit(handle);
    nvs_close(handle);
}
