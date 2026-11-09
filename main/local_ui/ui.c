/* Local front panel: band/channel state machine, spectrum, waterfall and a
 * channel strip on the ST7789 panel. Only one RF acquisition is in flight at
 * a time; the receiver's lock arbitrates with the host burst protocol. */
#include "local_ui.h"
#include "ui_backend.h"
#include "board_c5_lcd.h"
#include "st7789.h"
#include "font5x7.h"
#include "font7x12.h"
#include "settings.h"
#include "buttons.h"
#include "buzzer.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

#define FFT_BINS      256
#define RATE_CODE     2 /* 20 MS/s */
#define WF_STRIDE     320
#define WF_ROWS_MAX   120
#define MASK_BYTES    (320 * 140 / 4)
#define TEXT_PIXELS   (320 * 12)

#define COL_BG    0x0000
#define COL_PANEL 0x18C3
#define COL_GRID  0x2945
#define COL_DIM   0x8410
#define COL_TEXT  0xFFFF
#define COL_ACCENT 0x07E0
#define COL_PEAK  0xFFE0
#define COL_USB   0x07FF
#define COL_MUTE  0xF800
#define COL_CELL  0x2104

#define SWEEP_PERIOD_US  80000
#define RENDER_PERIOD_US 100000
#define HOST_PERIOD_US   500000

typedef struct {
    unsigned top_h, spec_y, spec_h, wf_y, wf_h, strip_y, strip_h;
} layout_t;

static ui_settings_t cfg;
static layout_t layout;
static uint8_t channel;    /* selected channel index inside the band */
static uint8_t scan_index; /* sweep position */
static uint8_t codes[FFT_BINS];
static uint8_t peak[FFT_BINS];
static bool have_data;
static bool muted;
static bool usb_active;
static bool full_clear = true;
static int last_step; /* +1 / -1: direction of the last channel step */
static int sel_x;
static int pk_db, act_db, det_pct;
static int64_t rot_start;
static bool rot_prev;
static uint8_t countdown_done;

static uint8_t *mask;
static uint8_t *wf;
static uint16_t *wf_line;
static uint16_t *text_buf;
static uint16_t wf_palette[256];
static unsigned wf_next;

static const uint16_t spec_palette[4] = {COL_BG, COL_GRID, COL_ACCENT, COL_PEAK};

static unsigned band_count(void) { return cfg.band == UI_BAND_5G ? 36 : 13; }
static unsigned band_number(unsigned idx) { return cfg.band == UI_BAND_5G ? 36 + idx * 4 : idx + 1; }
static unsigned band_mhz(unsigned idx) { return cfg.band == UI_BAND_5G ? 5180 + idx * 20 : 2412 + idx * 5; }

static void layout_build(void) {
    unsigned w = lcd_width(), h = lcd_height();
    if (w >= h) {
        layout = (layout_t){22, 22, 100, 122, 86, 208, 32};
    } else {
        layout = (layout_t){24, 24, 140, 164, 120, 284, 36};
    }
}

static void ui_text(unsigned x, unsigned y, const char *s, uint16_t fg, uint16_t bg, bool big) {
    if (!text_buf) return;
    unsigned gw = big ? FONT7X12_W : FONT5X7_W;
    unsigned gh = big ? FONT7X12_H : FONT5X7_H;
    unsigned cw = gw + 1;
    unsigned w_screen = lcd_width(), h_screen = lcd_height();
    unsigned len = (unsigned)strlen(s);
    if (!len || x >= w_screen || y >= h_screen) return;
    while (len && x + len * cw - 1 > w_screen) len--;
    if (!len) return;
    unsigned w = len * cw - 1;
    unsigned n = 0;
    for (unsigned row = 0; row < gh; row++) {
        for (unsigned i = 0; i < len; i++) {
            unsigned ch = (unsigned char)s[i];
            unsigned bits = 0;
            if (ch >= 32 && ch < 127) bits = big ? font7x12[ch - 32][row] : font5x7[ch - 32][row];
            for (unsigned col = 0; col < gw; col++) text_buf[n++] = ((bits >> col) & 1) ? fg : bg;
            if (i + 1 < len) text_buf[n++] = bg;
        }
    }
    lcd_set_window(x, y, w, gh);
    lcd_pixels(text_buf, w * gh);
}

static inline void mask_put(unsigned x, unsigned y, unsigned value) {
    unsigned stride = (lcd_width() + 3) / 4;
    uint8_t *p = &mask[y * stride + (x >> 2)];
    unsigned shift = 6 - 2 * (x & 3);
    *p = (uint8_t)((*p & ~(3u << shift)) | ((value & 3u) << shift));
}

static int level_of(unsigned code, unsigned h) {
    int db = (int)code / 2 - 84;
    int range = (int)cfg.top_db - (int)cfg.bottom_db;
    if (range < 5) range = 5;
    int level = (db - (int)cfg.bottom_db) * (int)h / range;
    if (level < 0) level = 0;
    if (level > (int)h) level = (int)h;
    return level;
}

static unsigned display_bin(unsigned x, unsigned w) { return (x * FFT_BINS / w + FFT_BINS / 2) % FFT_BINS; }

static void render_top(unsigned w) {
    lcd_fill_rect(0, 0, w, layout.top_h, COL_PANEL);
    char line[32];
    unsigned shown_mhz = usb_active ? sdr_ui_frequency() : band_mhz(channel);
    snprintf(line, sizeof(line), "%s CH%u", cfg.band == UI_BAND_5G ? "5G" : "2.4G", band_number(channel));
    ui_text(3, 2, line, COL_TEXT, COL_PANEL, false);
    snprintf(line, sizeof(line), "%s %d/%d", cfg.mode == UI_MODE_SWEEP ? "SWP" : "LIVE", cfg.top_db,
             cfg.bottom_db);
    ui_text(3, layout.top_h - 10, line, COL_DIM, COL_PANEL, false);
    snprintf(line, sizeof(line), "%u", shown_mhz);
    ui_text(104, (layout.top_h - FONT7X12_H) / 2, line, COL_TEXT, COL_PANEL, true);
    snprintf(line, sizeof(line), "PK%d", pk_db);
    ui_text(w - 34, 2, line, COL_PEAK, COL_PANEL, false);
    if (usb_active)
        ui_text(w - 34, layout.top_h - 10, "USB", COL_USB, COL_PANEL, false);
    else {
        snprintf(line, sizeof(line), "A%dD%d", act_db, det_pct);
        ui_text(w - 34, layout.top_h - 10, line, COL_TEXT, COL_PANEL, false);
    }
}

static void render_spectrum(unsigned w) {
    unsigned h = layout.spec_h;
    unsigned stride = (w + 3) / 4;
    memset(mask, 0, stride * h);
    for (unsigned y = 0; y < h; y += h / 4)
        for (unsigned x = 0; x < w; x++) mask_put(x, y, 1);
    for (unsigned x = 0; x < w; x += w / 8)
        for (unsigned y = 0; y < h; y++) mask_put(x, y, 1);
    if (have_data && !muted) {
        for (unsigned x = 0; x < w; x++) {
            unsigned bin = display_bin(x, w);
            int mean = level_of(codes[bin], h);
            int hold = level_of(peak[bin], h);
            if (hold < mean) hold = mean;
            for (int y = h - mean; y < (int)h; y++)
                if (y >= 0) mask_put(x, (unsigned)y, 2);
            for (int y = h - hold; y < h - mean; y++)
                if (y >= 0) mask_put(x, (unsigned)y, 3);
        }
    }
    lcd_draw_bitmap2(0, layout.spec_y, w, h, mask, spec_palette);
}

static void render_waterfall(unsigned w) {
    unsigned rows = layout.wf_h;
    for (unsigned r = 0; r < rows; r++) {
        const uint8_t *src = wf + ((wf_next + r) % rows) * WF_STRIDE;
        for (unsigned x = 0; x < w; x++) wf_line[x] = wf_palette[src[x]];
        lcd_set_window(0, layout.wf_y + r, w, 1);
        lcd_pixels(wf_line, w);
    }
}

static void render_strip(unsigned w) {
    lcd_fill_rect(0, layout.strip_y, w, layout.strip_h, COL_PANEL);
    unsigned count = band_count();
    unsigned cell = w / count;
    unsigned selected = usb_active ? (unsigned)((sel_x * (int)count) / (int)w)
                                   : (cfg.mode == UI_MODE_SWEEP ? scan_index : channel);
    if (selected >= count) selected = count - 1;
    char label[8];
    for (unsigned i = 0; i < count; i++) {
        unsigned x = i * cell;
        uint16_t fill = i == selected ? COL_ACCENT : COL_CELL;
        lcd_fill_rect(x + 1, layout.strip_y + 3, cell - 2, layout.strip_h - 6, fill);
        if (cell >= 16) {
            snprintf(label, sizeof(label), "%u", band_number(i));
            unsigned tw = (unsigned)strlen(label) * (FONT5X7_W + 1) - 1;
            ui_text(x + (cell - tw) / 2, layout.strip_y + (layout.strip_h - FONT5X7_H) / 2, label,
                    i == selected ? COL_BG : COL_DIM, fill, false);
        }
    }
    if (usb_active) {
        lcd_fill_rect(w / 2 - 18, layout.strip_y + 2, 36, layout.strip_h - 4, COL_PANEL);
        ui_text(w / 2 - 16, layout.strip_y + (layout.strip_h - FONT5X7_H) / 2, "USB", COL_USB, COL_PANEL,
                false);
    }
}

static void render_overlays(unsigned w) {
    if (muted) {
        lcd_fill_rect(w / 2 - 28, layout.spec_y + layout.spec_h / 2 - 6, 56, 13, COL_MUTE);
        ui_text(w / 2 - 24, layout.spec_y + layout.spec_h / 2 - 4, "MUTE", COL_TEXT, COL_MUTE, false);
    }
    if (!have_data && !usb_active) {
        unsigned y = layout.spec_y + layout.spec_h / 2 - FONT5X7_H / 2;
        ui_text(w / 2 - 27, y, "SCANNING", COL_DIM, COL_BG, false);
    }
    if (rot_prev && !usb_active) {
        int64_t elapsed = esp_timer_get_time() - rot_start;
        unsigned left = elapsed >= 5000000 ? 0 : (unsigned)((5000000 - elapsed + 999999) / 1000000);
        char line[16];
        snprintf(line, sizeof(line), "RESET %u", left);
        lcd_fill_rect(6, layout.spec_y + 4, 88, 15, COL_PANEL);
        ui_text(9, layout.spec_y + 7, line, COL_PEAK, COL_PANEL, false);
    }
}

static void render(void) {
    unsigned w = lcd_width();
    layout_build();
    if (full_clear) {
        lcd_fill(COL_BG);
        full_clear = false;
    }
    render_top(w);
    render_spectrum(w);
    render_waterfall(w);
    render_strip(w);
    render_overlays(w);
}

static void analyse(void) {
    uint8_t sorted[FFT_BINS];
    memcpy(sorted, codes, sizeof(sorted));
    for (unsigned i = 1; i < FFT_BINS; i++) {
        uint8_t v = sorted[i];
        unsigned j = i;
        while (j && sorted[j - 1] > v) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = v;
    }
    unsigned noise = sorted[FFT_BINS / 4];
    unsigned sum = 0, pk = 0, det = 0;
    for (unsigned j = 0; j < FFT_BINS; j++) {
        sum += codes[j];
        if (codes[j] > pk) pk = codes[j];
        if (codes[j] > noise + 16) det++;
    }
    pk_db = (int)(pk / 2) - 84;
    act_db = ((int)(sum / FFT_BINS) - (int)noise) / 2;
    det_pct = (int)(det * 100 / FFT_BINS);
}

static void waterfall_push(void) {
    unsigned w = lcd_width();
    unsigned rows = layout.wf_h;
    uint8_t *row = wf + wf_next * WF_STRIDE;
    for (unsigned x = 0; x < w; x++) row[x] = codes[display_bin(x, w)];
    wf_next = (wf_next + 1) % rows;
}

static void acquire(void) {
    unsigned count = band_count();
    if (cfg.mode == UI_MODE_SWEEP) {
        scan_index = (uint8_t)((scan_index + 1) % count);
        channel = scan_index;
    }
    unsigned mhz = band_mhz(channel);
    unsigned elapsed = 0;
    if (!sdr_ui_spectrum(mhz, FFT_BINS, RATE_CODE, codes, &elapsed)) return;
    have_data = true;
    for (unsigned j = 0; j < FFT_BINS; j++) {
        if (codes[j] >= peak[j])
            peak[j] = codes[j];
        else if (peak[j])
            peak[j]--;
    }
    analyse();
    waterfall_push();
    (void)elapsed;
}

static void change_channel(int direction) {
    unsigned count = band_count();
    channel = (uint8_t)((channel + (direction > 0 ? 1 : count - 1)) % count);
    scan_index = channel;
    last_step = direction > 0 ? 1 : -1;
    buzzer_play(UI_BUZZ_CHANNEL);
}

static void adjust_limit(bool top, int direction) {
    int8_t *limit = top ? &cfg.top_db : &cfg.bottom_db;
    int value = *limit + (direction > 0 ? 1 : -1);
    if (top) {
        if (value > -10) value = -10;
        if (value < cfg.bottom_db + 5) value = cfg.bottom_db + 5;
    } else {
        if (value < -120) value = -120;
        if (value > cfg.top_db - 5) value = cfg.top_db - 5;
    }
    *limit = value;
    ui_settings_save(&cfg);
    buzzer_play(top ? UI_BUZZ_TOP : UI_BUZZ_BOTTOM);
}

static void toggle_band(void) {
    cfg.band = cfg.band == UI_BAND_5G ? UI_BAND_24G : UI_BAND_5G;
    channel = scan_index = 0;
    memset(peak, 0, sizeof(peak));
    have_data = false;
    ui_settings_save(&cfg);
    buzzer_play(UI_BUZZ_BAND);
    full_clear = true;
}

static void toggle_mode(void) {
    cfg.mode = cfg.mode == UI_MODE_SWEEP ? UI_MODE_LIVE : UI_MODE_SWEEP;
    scan_index = channel;
    ui_settings_save(&cfg);
    buzzer_play(UI_BUZZ_MODE);
}

static void toggle_rotation(void) {
    cfg.landscape = cfg.landscape ? 0 : 1;
    lcd_set_rotation(cfg.landscape != 0);
    layout_build();
    wf_next = 0;
    ui_settings_save(&cfg);
    buzzer_play(UI_BUZZ_MODE);
    full_clear = true;
}

static void reset_defaults(void) {
    ui_settings_t fresh;
    ui_settings_defaults(&fresh);
    fresh.band = cfg.band;
    fresh.mode = cfg.mode;
    cfg = fresh;
    muted = false;
    buzzer_enable(true);
    channel = scan_index = 0;
    last_step = 0;
    lcd_set_rotation(cfg.landscape != 0);
    layout_build();
    wf_next = 0;
    memset(peak, 0, sizeof(peak));
    memset(wf, 0, WF_STRIDE * WF_ROWS_MAX);
    have_data = false;
    ui_settings_save(&cfg);
    buzzer_play(UI_BUZZ_RESET);
    full_clear = true;
}

static void toggle_mute(void) {
    muted = !muted;
    buzzer_enable(!muted);
    if (!muted) buzzer_play(UI_BUZZ_CHANNEL);
}

static void handle_events(const ui_button_event_t *event) {
    uint16_t left = (uint16_t)(1u << UI_BTN_LEFT), right = (uint16_t)(1u << UI_BTN_RIGHT);
    if (event->chord_mute) {
        toggle_mute();
        if (last_step) {
            unsigned count = band_count();
            channel = (uint8_t)((channel + (last_step > 0 ? count - 1 : 1)) % count);
            scan_index = channel;
            last_step = 0;
        }
    }
    int direction = 0;
    if (event->short_press & left) direction = -1;
    if (event->short_press & right) direction = 1;
    if (event->repeat & left) direction = -1;
    if (event->repeat & right) direction = 1;
    if (usb_active) {
        if (direction) {
            unsigned w = lcd_width();
            sel_x += direction * 8;
            if (sel_x < 1) sel_x = 1;
            if (sel_x > (int)w - 2) sel_x = (int)w - 2;
        }
    } else if (direction) {
        if (buttons_down(UI_BTN_BAND))
            adjust_limit(true, direction);
        else if (buttons_down(UI_BTN_MODE))
            adjust_limit(false, direction);
        else
            change_channel(direction);
    }
    if (!usb_active) {
        if (event->short_press & (1u << UI_BTN_BAND)) toggle_band();
        if (event->short_press & (1u << UI_BTN_MODE)) toggle_mode();
        if (event->short_press & (1u << UI_BTN_ROT)) toggle_rotation();
        if (event->long_press & (1u << UI_BTN_ROT)) reset_defaults();
    } else if (event->short_press & (1u << UI_BTN_ROT)) {
        toggle_rotation();
    }
}

static void countdown_poll(int64_t now) {
    bool down = buttons_down(UI_BTN_ROT);
    if (down && !rot_prev) {
        rot_start = now;
        countdown_done = 0;
    }
    if (down) {
        int64_t elapsed = now - rot_start;
        if (elapsed >= 3000000 && !(countdown_done & 1)) {
            countdown_done |= 1;
            buzzer_play(UI_BUZZ_COUNTDOWN);
        }
        if (elapsed >= 4000000 && !(countdown_done & 2)) {
            countdown_done |= 2;
            buzzer_play(UI_BUZZ_COUNTDOWN);
        }
    }
    rot_prev = down;
}

static void ui_task(void *arg) {
    (void)arg;
    int64_t last_acq = 0, last_render = 0;
    ui_button_event_t event;
    for (;;) {
        int64_t now = esp_timer_get_time();
        bool active = sdr_ui_host_active();
        if (active != usb_active) {
            usb_active = active;
            if (active) full_clear = false;
        }
        buttons_poll(&event);
        countdown_poll(now);
        handle_events(&event);
        if (!usb_active && now - last_acq >= SWEEP_PERIOD_US) {
            last_acq = now;
            acquire();
        }
        unsigned period = usb_active ? HOST_PERIOD_US : RENDER_PERIOD_US;
        if (now - last_render >= period) {
            last_render = now;
            if (!usb_active)
                sel_x = ((int)channel * (int)lcd_width() + lcd_width() / 2) / (int)band_count();
            render();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void palette_init(void) {
    for (unsigned i = 0; i < 256; i++) {
        uint8_t r = 0, g = 0, b = 0;
        if (i < 64) {
            b = (uint8_t)(i * 4);
        } else if (i < 128) {
            b = 255;
            g = (uint8_t)((i - 64) * 4);
        } else if (i < 192) {
            g = 255;
            b = (uint8_t)(255 - (i - 128) * 4);
            r = (uint8_t)((i - 128) * 4);
        } else {
            r = 255;
            g = (uint8_t)(255 - (i - 192) * 4);
        }
        wf_palette[i] = lcd_rgb565(r, g, b);
    }
}

void local_ui_start(void) {
    if (!lcd_init()) return;
    mask = heap_caps_malloc(MASK_BYTES, MALLOC_CAP_8BIT);
    wf = heap_caps_malloc(WF_STRIDE * WF_ROWS_MAX, MALLOC_CAP_8BIT);
    wf_line = heap_caps_malloc(320 * sizeof(uint16_t), MALLOC_CAP_8BIT);
    text_buf = heap_caps_malloc(TEXT_PIXELS * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (!mask || !wf || !wf_line || !text_buf) return;
    memset(wf, 0, WF_STRIDE * WF_ROWS_MAX);
    palette_init();

    ui_settings_load(&cfg);
    lcd_set_rotation(cfg.landscape != 0);
    layout_build();

    buttons_init();
    buzzer_init();
    buzzer_enable(cfg.buzz != 0);
    buzzer_play(UI_BUZZ_BOOT);

    channel = 0;
    scan_index = 0;
    xTaskCreate(ui_task, "local_ui", 6144, NULL, 3, NULL);
}
