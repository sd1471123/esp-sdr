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
    s->top_db = -26;
    s->bottom_db = -105;
    s->live_top_db = -26;
    s->live_bottom_db = -105;
}

static void sanitize(ui_settings_t *s) {
    if (s->band > UI_BAND_5G) s->band = UI_BAND_5G;
    if (s->mode > UI_MODE_LIVE) s->mode = UI_MODE_LIVE;
    if (s->landscape > 1) s->landscape = 1;
    if (s->buzz > 1) s->buzz = 1;
    if (s->mute > 1) s->mute = 1;
    s->top_db = clamp_db(s->top_db, -80, -10);
    s->bottom_db = clamp_db(s->bottom_db, -125, -55);
    if (s->bottom_db > s->top_db - 30) s->bottom_db = s->top_db - 30;
    s->live_top_db = clamp_db(s->live_top_db, -80, -10);
    s->live_bottom_db = clamp_db(s->live_bottom_db, -125, -55);
    if (s->live_bottom_db > s->live_top_db - 30) s->live_bottom_db = s->live_top_db - 30;
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
    int8_t d;
    if (nvs_get_i8(handle, "top", &d) == ESP_OK) s->top_db = d;
    if (nvs_get_i8(handle, "bot", &d) == ESP_OK) s->bottom_db = d;
    /* Upgrading from a blob that had one shared pair: let LIVE inherit it, then
     * override with the dedicated keys if they exist. */
    s->live_top_db = s->top_db;
    s->live_bottom_db = s->bottom_db;
    if (nvs_get_i8(handle, "ltop", &d) == ESP_OK) s->live_top_db = d;
    if (nvs_get_i8(handle, "lbot", &d) == ESP_OK) s->live_bottom_db = d;
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
    nvs_set_i8(handle, "top", s->top_db);
    nvs_set_i8(handle, "bot", s->bottom_db);
    nvs_set_i8(handle, "ltop", s->live_top_db);
    nvs_set_i8(handle, "lbot", s->live_bottom_db);
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
