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
#include <math.h>
#include <stdio.h>
#include <string.h>

#define FFT_BINS      256
#define RATE_CODE     2 /* 20 MS/s */
#define WF_STRIDE     320
#define WF_ROWS_MAX   120
#define MASK_BYTES    (320 * 140 / 4)
#define TEXT_PIXELS   (320 * 12)

/* Palette taken from the approved panel design (dark navy + cyan + amber). */
#define COL_BG      0x0862 /* #070b0f */
#define COL_SPEC_BG 0x0862 /* #080c11 */
#define COL_PANEL   0x10A3 /* #11161d */
#define COL_STRIP   0x0882 /* #0c1117 */
#define COL_GRID    0x18E4 /* rgba(255,255,255,.065) on spec bg */
#define COL_DIM     0x5B2E /* #5c6675 */
#define COL_MUTED   0xBE1A /* #b9c3d3 */
#define COL_TEXT    0xE75D /* #e8edf4 */
#define COL_ACCENT  0x063F /* #00c8ff */
#define COL_TRACE   0x069F /* #00d4ff */
#define COL_GLOW    0x030F /* cyan @35% */
#define COL_PEAK    0xFD89 /* #ffb347 */
#define COL_GREEN   0x6F76 /* #6ef0b0 */
#define COL_RED     0xFB4D /* #ff6b6b */
#define COL_TINT_C  0x0967 /* cyan 16% on strip bg */
#define COL_TINT_O  0x2924 /* amber 12% on panel */
#define COL_TINT_G  0x1A25 /* green 12% on panel */
#define COL_FILL    0x5A44 /* amber-brown area under the peak, like the phone app */
#define COL_USB     0x07FF
#define COL_MUTE    0xF800

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
/* Stitched band image, one entry per pixel column (same idea as the phone app): every
 * acquisition only refreshes the columns of the channel it captured, so the waterfall
 * keeps a fixed frequency axis while the sweep walks across it. */
static uint8_t band_codes[320];
static uint8_t band_peak[320];
/* Power accumulation for the running sweep, like the phone app: the trace is the
 * sweep average, the amber line is the max hold (2 dB decay per finished sweep). */
static float *spec_sum;
static uint16_t *spec_cnt;
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

static const uint16_t spec_palette[5] = {COL_SPEC_BG, COL_GRID, COL_TRACE, COL_PEAK, COL_FILL};
static uint8_t ch_pk[36], ch_act[36], ch_det[36];
static int8_t ch_pk_db[36], ch_act_db[36];
static uint64_t ch_seen; /* bit per channel with at least one capture */

/* Power average of the FFT bins covered by one pixel column. Without this the
 * 256-bin capture was sampled at a single bin per column, which left the stitched
 * trace far sparser and noisier than the phone app's. */
static uint8_t column_code(unsigned x, unsigned lo, unsigned hi, unsigned span) {
    unsigned b0 = (x - lo) * FFT_BINS / span;
    unsigned b1 = (x + 1 - lo) * FFT_BINS / span;
    if (b0 >= FFT_BINS) b0 = FFT_BINS - 1;
    if (b1 > FFT_BINS) b1 = FFT_BINS;
    if (b1 <= b0) b1 = b0 + 1;
    float p = 0.0f;
    for (unsigned b = b0; b < b1; b++) p += powf(10.0f, ((float)codes[b] * 0.5f - 84.0f) * 0.1f);
    float db = 10.0f * log10f(p / (float)(b1 - b0));
    int v = (int)((db + 84.0f) * 2.0f + 0.5f);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

static unsigned band_count(void) { return cfg.band == UI_BAND_5G ? 36 : 13; }
static unsigned band_number(unsigned idx) { return cfg.band == UI_BAND_5G ? 36 + idx * 4 : idx + 1; }
static unsigned band_mhz(unsigned idx) { return cfg.band == UI_BAND_5G ? 5180 + idx * 20 : 2412 + idx * 5; }

/* ch104 in 5G, ch7 in 2.4G: where the cursor and the sweep start (design spec). */
static unsigned middle_index(void) { return (band_count() - 1) / 2; }

static void layout_build(void) {
    unsigned w = lcd_width(), h = lcd_height();
    if (w >= h) {
        layout = (layout_t){30, 30, 94, 124, 84, 208, 32};
    } else {
        layout = (layout_t){30, 30, 134, 164, 120, 284, 36};
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

static unsigned text_w(const char *s) { return (unsigned)strlen(s) * (FONT5X7_W + 1) - 1; }

/* Filled rounded rectangle (radius <= 5), used for the mode pill, callout and
 * the selected channel marker. */
static void fill_round_rect(unsigned x, unsigned y, unsigned w, unsigned h, unsigned r, uint16_t color) {
    if (r > 5) r = 5;
    for (unsigned dy = 0; dy < h; dy++) {
        unsigned inset = 0;
        if (dy < r) {
            unsigned k = r - 1 - dy, s = 0;
            while ((s + 1) * (s + 1) <= r * r - k * k) s++;
            inset = r - s;
        } else if (dy >= h - r) {
            unsigned k = dy - (h - r), s = 0;
            while ((s + 1) * (s + 1) <= r * r - k * k) s++;
            inset = r - s;
        }
        if (inset * 2 >= w) continue;
        lcd_fill_rect(x + inset, y + dy, w - inset * 2, 1, color);
    }
}

static void round_rect_border(unsigned x, unsigned y, unsigned w, unsigned h, unsigned r, uint16_t color) {
    if (r > 5) r = 5;
    lcd_fill_rect(x + r, y, w - 2 * r, 1, color);
    lcd_fill_rect(x + r, y + h - 1, w - 2 * r, 1, color);
    lcd_fill_rect(x, y + r, 1, h - 2 * r, color);
    lcd_fill_rect(x + w - 1, y + r, 1, h - 2 * r, color);
    for (unsigned dy = 0; dy < r; dy++) {
        unsigned k = r - 1 - dy, s = 0;
        while ((s + 1) * (s + 1) <= r * r - k * k) s++;
        unsigned inset = r - s;
        lcd_fill_rect(x + inset, y + dy, 1, 1, color);
        lcd_fill_rect(x + w - 1 - inset, y + dy, 1, 1, color);
        lcd_fill_rect(x + inset, y + h - 1 - dy, 1, 1, color);
        lcd_fill_rect(x + w - 1 - inset, y + h - 1 - dy, 1, 1, color);
    }
}

static void draw_mute_icon(unsigned right, unsigned cy) {
    unsigned x = right - 11;
    lcd_fill_rect(x, cy - 1, 3, 3, COL_DIM);
    lcd_fill_rect(x + 2, cy - 3, 2, 7, COL_DIM);
    for (unsigned i = 0; i < 4; i++) {
        lcd_fill_rect(x + 6 + i, cy - 2 + i, 1, 1, COL_RED);
        lcd_fill_rect(x + 9 - i, cy - 2 + i, 1, 1, COL_RED);
    }
}

static unsigned text_w_big(const char *s) {
    return s[0] ? (unsigned)strlen(s) * (FONT7X12_W + 1) - 1 : 0;
}

static void render_top(unsigned w) {
    unsigned top = layout.top_h;
    unsigned ty = (top - FONT7X12_H) / 2;
    lcd_fill_rect(0, 0, w, top, COL_PANEL);
    lcd_fill_rect(0, top - 1, w, 1, COL_BG);

    const char *band = cfg.band == UI_BAND_5G ? "5G" : "2.4G";
    ui_text(4, ty, band, COL_TEXT, COL_PANEL, true);

    bool live = cfg.mode == UI_MODE_LIVE;
    uint16_t tint = live ? COL_TINT_G : COL_TINT_O;
    uint16_t edge = live ? COL_GREEN : COL_PEAK;
    unsigned bx = 4 + text_w_big(band) + 5;
    unsigned bw = 46, bh = FONT7X12_H + 8, by = (top - bh) / 2;
    fill_round_rect(bx, by, bw, bh, 4, tint);
    round_rect_border(bx, by, bw, bh, 4, edge);
    const char *mode = live ? "LIVE" : "SWEEP";
    ui_text(bx + (bw - text_w_big(mode)) / 2, ty, mode, edge, tint, true);

    int dpk = pk_db, dact = act_db, ddet = det_pct;
    if (cfg.mode == UI_MODE_SWEEP && ((ch_seen >> channel) & 1u)) {
        dpk = ch_pk_db[channel];
        dact = ch_act_db[channel];
        ddet = ch_det[channel];
    }
    char line[40];
    if (w >= 300)
        snprintf(line, sizeof(line), "PK%d A%d D%d%%", dpk, dact, ddet);
    else
        snprintf(line, sizeof(line), "P%d D%d%%", dpk, ddet);
    ui_text(bx + bw + 6, ty, line, COL_MUTED, COL_PANEL, true);

    unsigned shown = usb_active ? sdr_ui_frequency() : band_mhz(channel);
    if (w >= 300)
        snprintf(line, sizeof(line), "ch%u %uM", band_number(channel), shown);
    else
        snprintf(line, sizeof(line), "%u/%uM", band_number(channel), shown);
    unsigned tw = text_w_big(line);
    unsigned tx = (w > 4 + tw) ? w - 4 - tw : 0;
    ui_text(tx, ty, line, COL_TEXT, COL_PANEL, true);
    if (muted) draw_mute_icon(tx > 12 ? tx - 6 : w - 2, top / 2);
}


static void render_spectrum(unsigned w) {
    unsigned h = layout.spec_h;
    unsigned stride = (w + 3) / 4;
    lcd_fill_rect(0, layout.spec_y, w, h, COL_SPEC_BG);
    memset(mask, 0, stride * h);

    int range = (int)cfg.top_db - (int)cfg.bottom_db;
    if (range < 5) range = 5;

    for (int db = -120; db <= 0; db += 20) {
        if (db < cfg.bottom_db || db > cfg.top_db) continue;
        int y = (int)h - (db - (int)cfg.bottom_db) * (int)h / range;
        if (y < 0 || y >= (int)h) continue;
        for (unsigned x = 0; x < w; x++) mask_put(x, (unsigned)y, 1);
    }
    for (unsigned k = 1; k < 4; k++) {
        unsigned x = w * k / 4;
        for (unsigned y = 0; y < h; y++) mask_put(x, y, 1);
    }

    if (have_data) {
        int prev = -1;
        for (unsigned x = 0; x < w; x++) {
            int mean = level_of(band_codes[x], h);
            int hold = level_of(band_peak[x], h);
            if (hold < mean) hold = mean;
            int y = (int)h - 1 - mean;
            if (y < 0) y = 0;
            int ph = (int)h - 1 - hold;
            if (ph < 0) ph = 0;
            for (unsigned yy = (unsigned)ph; yy < h; yy++) mask_put(x, yy, 4);
            unsigned lo = (unsigned)(prev < 0 ? y : (prev < y ? prev : y));
            unsigned hi = (unsigned)(prev < 0 ? y : (prev > y ? prev : y));
            for (unsigned yy = lo; yy <= hi; yy++) mask_put(x, yy, 2);
            prev = y;
            mask_put(x, (unsigned)ph, 3);
        }
    }
    lcd_draw_bitmap2(0, layout.spec_y, w, h, mask, spec_palette);

    char line[32];
    for (int db = -120; db <= 0; db += 20) {
        if (db < cfg.bottom_db || db > cfg.top_db) continue;
        int y = (int)h - (db - (int)cfg.bottom_db) * (int)h / range;
        if (y <= 9 || y >= (int)h - 11) continue;
        snprintf(line, sizeof(line), "%d", db);
        ui_text(3, layout.spec_y + (unsigned)y - FONT5X7_H - 1, line, COL_DIM, COL_SPEC_BG, false);
    }

    unsigned shown = usb_active ? sdr_ui_frequency() : band_mhz(channel);
    unsigned lo_mhz, hi_mhz;
    if (usb_active) {
        lo_mhz = shown - 10;
        hi_mhz = shown + 10;
    } else if (cfg.mode == UI_MODE_SWEEP) {
        lo_mhz = band_mhz(0) - 10;
        hi_mhz = band_mhz(band_count() - 1) + 10;
    } else {
        lo_mhz = shown - 10;
        hi_mhz = shown + 10;
    }
    snprintf(line, sizeof(line), "%u", lo_mhz);
    ui_text(4, layout.spec_y + h - FONT5X7_H - 3, line, COL_DIM, COL_SPEC_BG, false);
    snprintf(line, sizeof(line), "%u", hi_mhz);
    unsigned tw = text_w(line);
    ui_text(w - 4 - tw, layout.spec_y + h - FONT5X7_H - 3, line, COL_DIM, COL_SPEC_BG, false);

    if (cfg.mode == UI_MODE_LIVE) {
        unsigned cx = w / 2;
        for (unsigned y = 0; y < h; y += 5) {
            unsigned seg = h - y < 3 ? h - y : 3;
            lcd_fill_rect(cx, layout.spec_y + y, 1, seg, COL_GLOW);
        }
        ui_text(4, layout.spec_y + 4, "LIVE 20M", COL_GLOW, COL_SPEC_BG, false);
    } else {
        char lab[40];
        int cdet = ((ch_seen >> channel) & 1u) ? ch_det[channel] : det_pct;
        snprintf(lab, sizeof(lab), "ch%u %uM  %d%%", band_number(channel), shown, cdet);
        unsigned lw = text_w(lab) + 10;
        fill_round_rect(3, layout.spec_y + 5, lw, 13, 3, 0x08A3);
        round_rect_border(3, layout.spec_y + 5, lw, 13, 3, COL_ACCENT);
        ui_text(8, layout.spec_y + 8, lab, 0xDFBF, 0x08A3, false);
    }
}

static void render_waterfall(unsigned w) {
    unsigned rows = layout.wf_h;
    int range = (int)cfg.top_db - (int)cfg.bottom_db;
    if (range < 5) range = 5;
    for (unsigned r = 0; r < rows; r++) {
        const uint8_t *src = wf + ((wf_next + r) % rows) * WF_STRIDE;
        for (unsigned x = 0; x < w; x++) {
            int db = (int)(src[x] / 2) - 84;
            int idx = (db - (int)cfg.bottom_db) * 255 / range;
            if (idx < 0) idx = 0;
            if (idx > 255) idx = 255;
            wf_line[x] = wf_palette[idx];
        }
        lcd_set_window(0, layout.wf_y + r, w, 1);
        lcd_pixels(wf_line, w);
    }
}

static void render_strip(unsigned w) {
    unsigned top = layout.strip_y;
    lcd_fill_rect(0, top, w, layout.strip_h, COL_STRIP);
    lcd_fill_rect(0, top, w, 1, COL_BG);
    unsigned count = band_count();
    unsigned margin = 4;
    unsigned gap = count > 20 ? 0 : 1;
    unsigned cw = (w - margin * 2 - gap * (count - 1)) / count;
    unsigned y = top + 3;
    unsigned h = layout.strip_h - 6;
    unsigned detH = 7, actH = 7, barGap = 2;
    unsigned actY = y + h - actH;
    unsigned detY = actY - barGap - detH;
    unsigned labelY = y + (detY - y - FONT5X7_H) / 2;
    unsigned selected = usb_active ? (unsigned)((sel_x * (int)count) / (int)w) : channel;
    if (selected >= count) selected = count - 1;
    char label[8];
    for (unsigned i = 0; i < count; i++) {
        unsigned x = margin + i * (cw + gap);
        if (i == selected) {
            fill_round_rect(x, y, cw, h, 2, COL_TINT_C);
            round_rect_border(x, y, cw, h, 2, COL_ACCENT);
        }
        unsigned track = cw > 2 ? cw - 2 : 1;
        unsigned det = ch_det[i], act = ch_act[i];
        lcd_fill_rect(x + 1, detY, track, detH, 0x2966);
        uint16_t dc = ch_pk[i] > 70 ? COL_RED : ch_pk[i] > 40 ? COL_PEAK : COL_GREEN;
        if (det > 2) {
            unsigned bwid = track * det / 100;
            lcd_fill_rect(x + 1, detY, bwid ? bwid : 1, detH, dc);
        }
        lcd_fill_rect(x + 1, actY, track, actH, 0x2105);
        if (act > 2) {
            unsigned bwid = track * act / 100;
            lcd_fill_rect(x + 1, actY, bwid ? bwid : 1, actH, COL_USB);
        }
        bool grp = count <= 13 || i % 4 == 0 || i + 1 == count;
        unsigned dist = selected > i ? selected - i : i - selected;
        if (grp && i != selected && (count <= 13 || dist > 1)) {
            snprintf(label, sizeof(label), "%u", band_number(i));
            unsigned tw = text_w(label);
            int lx = (int)(x + cw / 2) - (int)(tw / 2);
            if (lx < 0) lx = 0;
            ui_text((unsigned)lx, labelY, label, COL_DIM, COL_STRIP, false);
        }
    }
    {
        unsigned x = margin + selected * (cw + gap);
        snprintf(label, sizeof(label), "%u", band_number(selected));
        unsigned tw = text_w(label);
        int lx = (int)(x + cw / 2) - (int)(tw / 2);
        if (lx < 0) lx = 0;
        ui_text((unsigned)lx, labelY, label, COL_TEXT, COL_TINT_C, false);
    }
    if (usb_active) {
        lcd_fill_rect(w / 2 - 18, top + 2, 36, layout.strip_h - 4, COL_STRIP);
        ui_text(w / 2 - 16, top + (layout.strip_h - FONT5X7_H) / 2, "USB", COL_USB, COL_STRIP, false);
    }
}

static void render_overlays(unsigned w) {
    (void)w;
    if (rot_prev && !usb_active) {
        int64_t elapsed = esp_timer_get_time() - rot_start;
        unsigned left = elapsed >= 5000000 ? 0 : (unsigned)((5000000 - elapsed + 999999) / 1000000);
        char line[16];
        snprintf(line, sizeof(line), "RESET %u", left);
        fill_round_rect(6, layout.spec_y + 4, 88, 15, 3, COL_PANEL);
        round_rect_border(6, layout.spec_y + 4, 88, 15, 3, COL_PEAK);
        ui_text(9, layout.spec_y + 8, line, COL_PEAK, COL_PANEL, false);
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

/* Recompute the per-channel bars from the finished sweep: the app only publishes
 * the bars/rates when a pass completes, so the strip shows sweep results rather
 * than a sample of whichever window was just captured. */
static void rebuild_channel_stats(void) {
    unsigned w = lcd_width(), count = band_count();
    if (!count) return;
    unsigned sample[64];
    for (unsigned k = 0; k < 64; k++) sample[k] = band_codes[(k * (w - 1)) / 63];
    for (unsigned i = 1; i < 64; i++) {
        unsigned v = sample[i], j = i;
        while (j && sample[j - 1] > v) {
            sample[j] = sample[j - 1];
            j--;
        }
        sample[j] = v;
    }
    unsigned noise = sample[16];
    for (unsigned c = 0; c < count && c < 36; c++) {
        unsigned lo = c * w / count, hi = (c + 1) * w / count;
        if (hi <= lo) hi = lo + 1;
        unsigned pk = 0, sum = 0, det = 0, n = 0;
        for (unsigned x = lo; x < hi && x < w; x++) {
            unsigned v = band_peak[x];
            if (v > pk) pk = v;
            sum += band_codes[x];
            if (band_codes[x] > noise + 16) det++;
            n++;
        }
        if (!n) continue;
        int pk_db_c = (int)(pk / 2) - 84;
        int act_db_c = ((int)(sum / n) - (int)noise) / 2;
        int lv = (pk_db_c - (int)(cfg.bottom_db + 4)) * 100 / 50;
        ch_pk[c] = (uint8_t)(lv < 0 ? 0 : lv > 100 ? 100 : lv);
        int ac = act_db_c * 100 / 60;
        ch_act[c] = (uint8_t)(ac < 0 ? 0 : ac > 100 ? 100 : ac);
        ch_det[c] = (uint8_t)(det * 100 / n);
        ch_pk_db[c] = (int8_t)pk_db_c;
        ch_act_db[c] = (int8_t)act_db_c;
        ch_seen |= (uint64_t)1 << c;
    }
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

static void stats_store(unsigned i) {
    if (i >= 36) return;
    int lv = (pk_db - (cfg.bottom_db + 4)) * 100 / 50;
    int ac = act_db * 100 / 60;
    int de = det_pct;
    if (lv < 0) lv = 0;
    if (lv > 100) lv = 100;
    if (ac < 0) ac = 0;
    if (ac > 100) ac = 100;
    if (de < 0) de = 0;
    if (de > 100) de = 100;
    ch_pk[i] = (uint8_t)lv;
    ch_act[i] = (uint8_t)ac;
    ch_det[i] = (uint8_t)de;
    ch_pk_db[i] = (int8_t)pk_db;
    ch_act_db[i] = (int8_t)act_db;
    ch_seen |= (uint64_t)1 << i;
}

static void waterfall_push(void) {
    unsigned w = lcd_width();
    unsigned rows = layout.wf_h;
    uint8_t *row = wf + wf_next * WF_STRIDE;
    if (w > WF_STRIDE) w = WF_STRIDE;
    memcpy(row, band_codes, w);
    wf_next = (wf_next + 1) % rows;
}

static void acquire(void) {
    unsigned w = lcd_width();
    unsigned count = band_count();
    bool sweep = cfg.mode == UI_MODE_SWEEP;
    if (sweep) {
        scan_index = (uint8_t)((scan_index + 1) % count);
        if (scan_index == 0) {
            /* one full pass done: the phone app drops PEAK_DECAY_DB (2 dB) from the hold */
            for (unsigned x = 0; x < w; x++)
                if (band_peak[x]) band_peak[x]--;
            if (spec_sum) memset(spec_sum, 0, 320 * sizeof(float));
            if (spec_cnt) memset(spec_cnt, 0, 320 * sizeof(uint16_t));
            rebuild_channel_stats();
        }
    }
    unsigned tuned = sweep ? scan_index : channel;
    unsigned mhz = band_mhz(tuned);
    unsigned elapsed = 0;
    if (!sdr_ui_spectrum(mhz, FFT_BINS, RATE_CODE, codes, &elapsed)) return;
    have_data = true;

    unsigned lo = sweep ? (unsigned)tuned * w / count : 0;
    unsigned hi = sweep ? (unsigned)(tuned + 1) * w / count : w;
    if (hi <= lo) hi = lo + 1;
    if (hi > w) hi = w;
    unsigned span = hi - lo;
    for (unsigned x = lo; x < hi; x++) {
        uint8_t v = column_code(x, lo, hi, span);
        if (sweep && spec_sum && spec_cnt) {
            spec_sum[x] += powf(10.0f, ((float)v * 0.5f - 84.0f) * 0.1f);
            spec_cnt[x]++;
            float avg_db = 10.0f * log10f(spec_sum[x] / (float)spec_cnt[x]);
            int a = (int)((avg_db + 84.0f) * 2.0f + 0.5f);
            if (a < 0) a = 0;
            if (a > 255) a = 255;
            band_codes[x] = (uint8_t)a;
        } else {
            band_codes[x] = v;
        }
        if (v >= band_peak[x]) band_peak[x] = v;
    }
    analyse();
    if (sweep)
        stats_store(tuned); /* only for the per-channel dB history of the cursor */
    else
        rebuild_channel_stats();
    waterfall_push();
    (void)elapsed;
}

static void change_channel(int direction) {
    unsigned count = band_count();
    channel = (uint8_t)((channel + (direction > 0 ? 1 : count - 1)) % count);
    last_step = direction > 0 ? 1 : -1;
    buzzer_play(UI_BUZZ_CHANNEL);
}

static void adjust_limit(bool top, int direction) {
    int8_t *limit = top ? &cfg.top_db : &cfg.bottom_db;
    int value = *limit + (direction > 0 ? 5 : -5);
    if (top) {
        if (value > -10) value = -10;
        if (value < -80) value = -80;
        if (value < cfg.bottom_db + 30) value = cfg.bottom_db + 30;
    } else {
        if (value < -125) value = -125;
        if (value > -55) value = -55;
        if (value > cfg.top_db - 30) value = cfg.top_db - 30;
    }
    *limit = value;
    ui_settings_save(&cfg);
    buzzer_play(top ? UI_BUZZ_TOP : UI_BUZZ_BOTTOM);
}

static void toggle_band(void) {
    cfg.band = cfg.band == UI_BAND_5G ? UI_BAND_24G : UI_BAND_5G;
    channel = (uint8_t)middle_index();
    scan_index = 0; /* start the pass at the first channel so every wrap is a full pass */
    memset(band_codes, 0, sizeof(band_codes));
    memset(band_peak, 0, sizeof(band_peak));
    memset(wf, 0, WF_STRIDE * WF_ROWS_MAX);
    wf_next = 0;
    memset(ch_pk, 0, sizeof(ch_pk));
    memset(ch_act, 0, sizeof(ch_act));
    memset(ch_det, 0, sizeof(ch_det));
    memset(ch_pk_db, 0, sizeof(ch_pk_db));
    memset(ch_act_db, 0, sizeof(ch_act_db));
    ch_seen = 0;
    have_data = false;
    ui_settings_save(&cfg);
    buzzer_play(UI_BUZZ_BAND);
    full_clear = true;
}

static void toggle_mode(void) {
    cfg.mode = cfg.mode == UI_MODE_SWEEP ? UI_MODE_LIVE : UI_MODE_SWEEP;
    if (cfg.mode == UI_MODE_SWEEP) channel = (uint8_t)middle_index();
    scan_index = cfg.mode == UI_MODE_SWEEP ? 0 : channel;
    memset(band_codes, 0, sizeof(band_codes));
    memset(band_peak, 0, sizeof(band_peak));
    memset(wf, 0, WF_STRIDE * WF_ROWS_MAX);
    wf_next = 0;
    have_data = false;
    ui_settings_save(&cfg);
    buzzer_play(UI_BUZZ_MODE);
    full_clear = true;
}

static void toggle_rotation(void) {
    cfg.landscape = cfg.landscape ? 0 : 1;
    lcd_set_rotation(cfg.landscape != 0);
    layout_build();
    wf_next = 0;
    memset(band_codes, 0, sizeof(band_codes));
    memset(band_peak, 0, sizeof(band_peak));
    memset(wf, 0, WF_STRIDE * WF_ROWS_MAX);
    have_data = false;
    ui_settings_save(&cfg);
    buzzer_play(UI_BUZZ_MODE);
    full_clear = true;
}

static void reset_defaults(void) {
    ui_settings_t fresh;
    ui_settings_defaults(&fresh);
    cfg = fresh;
    muted = false;
    buzzer_enable(true);
    channel = (uint8_t)middle_index();
    scan_index = 0;
    last_step = 0;
    lcd_set_rotation(cfg.landscape != 0);
    layout_build();
    wf_next = 0;
    memset(band_codes, 0, sizeof(band_codes));
    memset(band_peak, 0, sizeof(band_peak));
    memset(ch_pk, 0, sizeof(ch_pk));
    memset(ch_act, 0, sizeof(ch_act));
    memset(ch_det, 0, sizeof(ch_det));
    memset(wf, 0, WF_STRIDE * WF_ROWS_MAX);
    have_data = false;
    ui_settings_clear();
    buzzer_play(UI_BUZZ_RESET);
    full_clear = true;
}

static void toggle_mute(void) {
    muted = !muted;
    cfg.mute = muted ? 1 : 0;
    ui_settings_save(&cfg);
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
        if (event->dir_mod == 1)
            adjust_limit(true, direction);
        else if (event->dir_mod == 2)
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
    /* Cool-to-hot gradient from the approved design: navy, teal, green, amber, red. */
    static const uint8_t stops[8][3] = {
        {16, 42, 67},   {14, 76, 107},  {10, 124, 138}, {18, 179, 168},
        {142, 212, 60}, {255, 210, 63}, {255, 140, 66}, {232, 70, 58},
    };
    for (unsigned i = 0; i < 256; i++) {
        unsigned pos = i * 7 * 256 / 255;
        unsigned seg = pos >> 8;
        unsigned f = pos & 0xff;
        if (seg > 6) {
            seg = 6;
            f = 255;
        }
        uint8_t r = (uint8_t)((stops[seg][0] * (256 - f) + stops[seg + 1][0] * f) >> 8);
        uint8_t g = (uint8_t)((stops[seg][1] * (256 - f) + stops[seg + 1][1] * f) >> 8);
        uint8_t b = (uint8_t)((stops[seg][2] * (256 - f) + stops[seg + 1][2] * f) >> 8);
        wf_palette[i] = lcd_rgb565(r, g, b);
    }
}

void local_ui_start(void) {
    if (!lcd_init()) return;
    mask = heap_caps_malloc(MASK_BYTES, MALLOC_CAP_8BIT);
    spec_sum = heap_caps_malloc(320 * sizeof(float), MALLOC_CAP_8BIT);
    spec_cnt = heap_caps_malloc(320 * sizeof(uint16_t), MALLOC_CAP_8BIT);
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
    buzzer_enable(cfg.buzz != 0 && !cfg.mute);
    buzzer_play(UI_BUZZ_BOOT);

    muted = cfg.mute != 0;
    channel = (uint8_t)middle_index();
    scan_index = 0;
    xTaskCreate(ui_task, "local_ui", 6144, NULL, 3, NULL);
}
