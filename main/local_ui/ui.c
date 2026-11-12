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
#include "driver/uart.h"
#include "sdkconfig.h"
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Bring-up trace. The panel image puts its console on the USB-Serial-JTAG
 * port, which is exactly the port that goes away when a wiring experiment
 * pulls the USB pins low, so the trace is written straight to UART0 instead:
 * the CH340 header is always readable.
 *
 * The diagnostic build drops the host UART to 115200 (see
 * sdkconfig.defaults.esp32c5-lcd-diag), so it gets the trace for free. A
 * normal build runs the burst protocol on UART0 at 2 Mbaud: printing UI text
 * there would corrupt the host stream, so the trace is off. Override with
 * -DUI_TRACE=1 if a debug session needs it anyway. */
#ifndef UI_TRACE
#if CONFIG_ESP_SDR_UART_BAUD <= 115200
#define UI_TRACE 1
#else
#define UI_TRACE 0
#endif
#endif

#if UI_TRACE
static void trace(const char *fmt, ...) {
    static bool started;
    if (!started) {
        const uart_config_t cfg = {
            .baud_rate = 115200, .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
        };
        /* burst_serial_init() usually owns UART0 already. Take it over when it
         * is free, but never bail out: if the install fails the driver is live
         * at the configured host baud and the write below still lands. */
        if (uart_driver_install(UART_NUM_0, 512, 1024, 0, NULL, 0) == ESP_OK) {
            uart_param_config(UART_NUM_0, &cfg);
            uart_set_pin(UART_NUM_0, 11, 12, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        }
        started = true;
    }
    char line[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0) uart_write_bytes(UART_NUM_0, line, (size_t)n);
}
#else
static void trace(const char *fmt, ...) { (void)fmt; }
#endif

#define FFT_BINS      256
#define RATE_CODE     2 /* 20 MS/s */
#define RATE_SWEEP    0 /* 80 MS/s, same capture width as the phone app */
#define WF_STRIDE     320
/* Rows of history kept for the waterfall. 120 rows cost 75 KB of internal
 * RAM, which on this build left 6 KB and made xTaskCreate() fail outright —
 * the panel then never drew and the keys were never polled. 64 rows still
 * covers the visible 62 px strip. */
#define WF_ROWS_MAX   64
#define MASK_BYTES    (320 * 140 / 4)
#define TEXT_PIXELS   (320 * 12)

/* Palette taken from the approved panel design (dark navy + cyan + amber).
 * The drawing areas are pure black: an IPS panel's black level is the floor of
 * the whole picture, so anything above 0x0000 reads as "washed out grey" once
 * the backlight is at full brightness. Only the status bar and the channel
 * strip keep a single 5-6-5 step of lift so their edges stay visible. */
#define COL_BG      0x0000 /* #000000 */
#define COL_SPEC_BG 0x0000 /* #000000 */
#define COL_PANEL   0x0862 /* #080c10 — one step above black */
#define COL_STRIP   0x0862 /* #080c10 */
#define COL_GRID    0x10A3 /* faint grid on black */
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
#define COL_USB     0x07FF
#define COL_MUTE    0xF800

/* Minimum spacing between two sweep captures.
 *
 * Measured on hardware: the RF side of a step costs 1-2 ms (prepare_rx, retune,
 * adc trigger and the sentinel check), so this constant is not protecting the
 * radio — it only paces the capture slots the renderer offers. Those slots sit
 * about 10 ms apart inside the waterfall, so the previous 80 ms and even 30 ms
 * values threw away most of them: a step ended up waiting for the slot grid
 * instead of for the radio. Keep this below the slot spacing so nearly every
 * slot captures.
 */
#define SWEEP_PERIOD_US  5000
/* A frame is ~134 KB: ~270 ms at 4 MHz, ~135 ms at 8 MHz. Asking for less than
 * the frame time made render() start the next frame the instant it returned, so
 * the panel never got a still picture and the write seam never stopped moving.
 * 300 ms leaves a real stable window at either clock. The RF engine still gets
 * slots at the frame boundaries, so the sweep keeps its 80 ms cadence. */
#define RENDER_PERIOD_US 300000
#define HOST_PERIOD_US   500000

typedef struct {
    unsigned top_h, spec_y, spec_h, wf_y, wf_h, strip_y, strip_h;
} layout_t;

/* Defined with the channel strip below; the spectrum uses it so the selection
 * box and the strip cursor land on exactly the same frequency column. */
static unsigned strip_x(unsigned mhz, unsigned w);

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

/* lcd_draw_bitmap2() reads a 2-bit index, so only these four entries are
 * reachable. A fifth "peak wash" colour used to sit at index 4 and was silently
 * masked to 0 by mask_put(), which erased the grid under the peak band instead
 * of tinting it. The band now shares the grid colour, the same near-black wash. */
static const uint16_t spec_palette[4] = {COL_SPEC_BG, COL_GRID, COL_TRACE, COL_PEAK};
static uint8_t ch_pk[36], ch_act[36], ch_det[36];
static int8_t ch_pk_db[36], ch_act_db[36];
static uint64_t ch_seen;    /* bit per channel with at least one capture */
static uint16_t ch_hits[36], sweeps;
static uint8_t *win_peak; /* peak hold of the 20 MHz window on screen (heap) */
static uint8_t last_tuned;         /* channel whose window is on screen */
static uint8_t col_seen[40];        /* bitmap: band column covered by this sweep */
static unsigned last_acq_ms;        /* whole-step cost of the most recent capture */
static unsigned last_frame_ms;      /* wall time of the most recent repaint */
static unsigned acq_ok, acq_fail;   /* capture attempts vs. successful ones */
static unsigned sweep_step_count;   /* monotonic, so the tick can report a true rate */

/* Vertical scale, as in the phone app: a 70 dB window anchored on the noise floor
 * (floor + 55) or on the strongest peak, attack 0.4 / release 0.06 per capture. */
static int view_top_db = -30, view_bot_db = -100;
/* Noise floor the auto-range settled on, published for the diagnostics line. */
static int floor_db_seen = -84;


/* Band plans copied from the phone app (Bands.range / channels20). */
/* The axis spans exactly the 20 MHz blocks of the first and last channel, so a
 * channel's block always starts at column 0 and ends at column w. The old 5 GHz
 * range started at 5150 while ch36's block starts at 5170, which left ~12 px of
 * dead space on the left of the strip but none on the right: the bars looked
 * flush right and short left. 2400/2500 did the same thing to 2.4 GHz, worse at
 * the right edge (ch13 ends at 2482). */
static unsigned band_lo(void) { return cfg.band == UI_BAND_5G ? 5170 : 2402; }
static unsigned band_hi(void) { return cfg.band == UI_BAND_5G ? 5895 : 2482; }

/* Standard 20 MHz channel table (IEEE 802.11), the same one the panel design
 * uses. The 5 GHz numbering is *not* a uniform 20 MHz grid: ch64 ends at 5320
 * and the next usable centre is 5480 (ch96), and UNII-3 sits on half-steps
 * (ch149 = 5745, not 5740). Deriving the number as 36 + idx * 4 therefore
 * invented channels 68…92 and 148…176 that do not exist, which is what the
 * strip was showing. Look the number up instead of computing it. */
typedef struct {
    uint8_t num;
    uint16_t mhz;
} chan_t;

static const chan_t k_ch24[] = {
    {1, 2412},  {2, 2417},  {3, 2422},  {4, 2427},  {5, 2432},  {6, 2437}, {7, 2442},
    {8, 2447},  {9, 2452},  {10, 2457}, {11, 2462}, {12, 2467}, {13, 2472},
};
static const chan_t k_ch5g[] = {
    {36, 5180},  {40, 5200},  {44, 5220},  {48, 5240},
    {52, 5260},  {56, 5280},  {60, 5300},  {64, 5320},
    {96, 5480}, /* UNII-2C lower edge: legal IEEE channel, almost never enabled */
    {100, 5500}, {104, 5520}, {108, 5540}, {112, 5560}, {116, 5580},
    {120, 5600}, {124, 5620}, {128, 5640}, {132, 5660}, {136, 5680},
    {140, 5700}, {144, 5720},
    {149, 5745}, {153, 5765}, {157, 5785}, {161, 5805}, {165, 5825},
    {169, 5845}, {173, 5865}, {177, 5885},
};
#define CH24_COUNT (sizeof(k_ch24) / sizeof(k_ch24[0]))
#define CH5G_COUNT (sizeof(k_ch5g) / sizeof(k_ch5g[0]))

static const chan_t *band_tab(void) { return cfg.band == UI_BAND_5G ? k_ch5g : k_ch24; }
static unsigned band_count(void) { return cfg.band == UI_BAND_5G ? CH5G_COUNT : CH24_COUNT; }
static unsigned band_number(unsigned idx) {
    return idx < band_count() ? band_tab()[idx].num : 0;
}
static unsigned band_mhz(unsigned idx) { return idx < band_count() ? band_tab()[idx].mhz : 0; }

/* Stretches between Wi-Fi channels that have no channel number of their own.
 * 5330-5350 is radar-only inside the DFS band, 5350-5470 is satellite active
 * sensing (Sentinel-1 SAR sits at 5405), and the 5 MHz sliver at 5730-5735 falls
 * inside the ISM band. They still get measured and shown, only the label is the
 * band's purpose instead of a channel number. */
typedef struct {
    uint16_t lo, hi;
    const char *tag;
} gap_t;

static const gap_t k_gaps5g[] = {
    {5330, 5350, "RADAR"},
    {5350, 5470, "SAT"},
    {5730, 5735, "ISM"},
};
#define GAP_COUNT (sizeof(k_gaps5g) / sizeof(k_gaps5g[0]))

static unsigned gap_count(void) { return cfg.band == UI_BAND_5G ? GAP_COUNT : 0; }
static unsigned gap_lo(unsigned i) { return k_gaps5g[i].lo; }
static unsigned gap_hi(unsigned i) { return k_gaps5g[i].hi; }
static const char *gap_tag(unsigned i) { return k_gaps5g[i].tag; }

/* Same three numbers as the Wi-Fi channels, for the stretches that have no
 * channel number. Kept tiny: BSS is already fighting the C5 RF dump window. */
static uint8_t gap_pk[GAP_COUNT], gap_act[GAP_COUNT], gap_det[GAP_COUNT];
static uint16_t gap_hits[GAP_COUNT];

/* The sweep steps its LO every 20 MHz from band start - 10 to band end + 10. */
static unsigned sweep_steps(void) { return (band_hi() + 10 - (band_lo() - 10)) / 20 + 1; }
static unsigned sweep_lo(unsigned i) { return band_lo() - 10 + i * 20; }

/* Pixel column that shows a frequency on the band axis, or -1 outside it. */
static int column_of_f(float mhz, unsigned w) {
    float span = (float)band_hi() - (float)band_lo();
    if (span <= 0 || w < 2) return -1;
    int c = (int)((mhz - (float)band_lo()) * (float)(w - 1) / span);
    return (c < 0 || c >= (int)w) ? -1 : c;
}
static int column_of(unsigned mhz, unsigned w) { return column_of_f((float)mhz, w); }

static bool col_seen_bit(unsigned x) { return (col_seen[x >> 3] >> (x & 7)) & 1u; }
static void col_seen_set(unsigned x) { col_seen[x >> 3] |= (uint8_t)(1u << (x & 7)); }

static int level_of_code(unsigned code, unsigned h) {
    int range = view_top_db - view_bot_db;
    if (range < 5) range = 5;
    int db = (int)code / 2 - 84;
    int v = (db - view_bot_db) * (int)h / range;
    if (v < 0) v = 0;
    return v > (int)h - 1 ? (int)h - 1 : v;
}

/* Map an n-entry capture onto w pixels with linear interpolation. */
static uint8_t lerp8(const uint8_t *src, unsigned n, unsigned x, unsigned w) {
    if (n < 2 || w < 2) return src[0];
    unsigned num = x * (n - 1), den = w - 1;
    unsigned i = num / den, f = (num % den) * 256 / den;
    unsigned a = src[i], b = src[i + 1 < n ? i + 1 : i];
    return (uint8_t)((a * (256 - f) + b * f) >> 8);
}

/* Trace value for a pixel: band columns while sweeping, the live window otherwise. */
static uint8_t disp_code(unsigned x, unsigned w) {
    if (cfg.mode == UI_MODE_SWEEP || usb_active) return band_codes[x];
    return lerp8(codes, FFT_BINS, x, w);
}
static uint8_t disp_hold(unsigned x, unsigned w) {
    if (cfg.mode == UI_MODE_SWEEP || usb_active) return band_peak[x];
    return win_peak ? lerp8(win_peak, FFT_BINS, x, w) : 0;
}

/* Load the vertical limits for the exact band + mode on screen. All four
 * combinations own their own pair, so switching band, mode, or leaving a limit
 * chord must re-point the view immediately. No auto-range anywhere. */
static unsigned scale_band(void) {
    return cfg.band == UI_BAND_5G ? UI_BAND_5G : UI_BAND_24G;
}

static void apply_stored_scale(void) {
    unsigned b = scale_band();
    bool live = cfg.mode == UI_MODE_LIVE;
    view_top_db = live ? cfg.live_top_db[b] : cfg.top_db[b];
    view_bot_db = live ? cfg.live_bottom_db[b] : cfg.bottom_db[b];
}

/* There is no auto-range on either mode. Both use their own stored dB limits,
 * applied by apply_stored_scale() at every point where the mode or those limits
 * can change: boot, mode/band switch, a limit chord, factory reset. The old
 * noise-floor follower made the grid and trace drift on every capture, which is
 * exactly why the LIVE axis looked unstable. */

/* ch104 in 5G, ch7 in 2.4G: where the cursor and the sweep start (design spec). */
static unsigned middle_index(void) { return (band_count() - 1) / 2; }

static void layout_build(void) {
    unsigned w = lcd_width(), h = lcd_height();
    /* the channel strip only exists while sweeping; live gets the rows back */
    bool strip = cfg.mode == UI_MODE_SWEEP || usb_active;
    if (w >= h)
        layout = strip ? (layout_t){30, 30, 94, 124, 84, 208, 32}
                       : (layout_t){30, 30, 108, 138, 102, 240, 0};
    else
        layout = strip ? (layout_t){30, 30, 134, 164, 120, 284, 36}
                       : (layout_t){30, 30, 150, 180, 140, 320, 0};
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

/* Text drawn *into* the bitmap instead of on top of it.
 *
 * Anything painted over the spectrum with ui_text() was erased again by the
 * next frame's bitmap pass and only came back at the very end of it, so labels
 * and the callout blinked at frame rate. Composing them into the mask means the
 * whole region is written exactly once per frame and nothing disappears. */
static void mask_text(unsigned x, unsigned y, const char *s, unsigned idx) {
    unsigned sw = lcd_width();
    unsigned n = (unsigned)strlen(s), cw = FONT5X7_W + 1;
    for (unsigned row = 0; row < FONT5X7_H; row++) {
        for (unsigned i = 0; i < n; i++) {
            unsigned ch = (unsigned char)s[i];
            unsigned bits = (ch >= 32 && ch < 127) ? font5x7[ch - 32][row] : 0;
            for (unsigned col = 0; col < FONT5X7_W; col++) {
                if (!((bits >> col) & 1)) continue;
                unsigned px = x + i * cw + col;
                if (px < sw) mask_put(px, y + row, idx);
            }
        }
    }
}

static void mask_rect(unsigned x, unsigned y, unsigned w, unsigned h, unsigned idx) {
    for (unsigned dy = 0; dy < h; dy++)
        for (unsigned dx = 0; dx < w; dx++) mask_put(x + dx, y + dy, idx);
}

/* 7x12 variant for the status bar. Composing the whole bar into the mask and
 * sending it in one window is what finally stopped it flickering: the old
 * repaint cleared 30 rows and then opened six or seven tiny windows for the
 * pill, labels and icon, so the panel's scan kept cutting across the middle of
 * a half-rebuilt bar. */
static void mask_text_big(unsigned x, unsigned y, const char *s, unsigned idx) {
    unsigned sw = lcd_width();
    unsigned n = (unsigned)strlen(s), cw = FONT7X12_W + 1;
    for (unsigned row = 0; row < FONT7X12_H; row++) {
        for (unsigned i = 0; i < n; i++) {
            unsigned ch = (unsigned char)s[i];
            unsigned bits = (ch >= 32 && ch < 127) ? font7x12[ch - 32][row] : 0;
            for (unsigned col = 0; col < FONT7X12_W; col++) {
                if (!((bits >> col) & 1)) continue;
                unsigned px = x + i * cw + col;
                if (px < sw) mask_put(px, y + row, idx);
            }
        }
    }
}

static void mask_mute_icon(unsigned right, unsigned cy) {
    unsigned x = right - 11;
    mask_rect(x, cy - 1, 3, 3, 1);
    mask_rect(x + 2, cy - 3, 2, 7, 1);
    for (unsigned i = 0; i < 4; i++) {
        mask_put(x + 6 + i, cy - 2 + i, 2);
        mask_put(x + 9 - i, cy - 2 + i, 2);
    }
}

static int level_of(unsigned code, unsigned h) {
    int db = (int)code / 2 - 84;
    int range = view_top_db - view_bot_db;
    if (range < 5) range = 5;
    int level = (db - view_bot_db) * (int)h / range;
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

static unsigned text_w_big(const char *s) {
    return s[0] ? (unsigned)strlen(s) * (FONT7X12_W + 1) - 1 : 0;
}

/* Keys are sampled from inside the renderer as well as from the main loop.
 * A full repaint occupies the task for ~0.3 s, and a key that goes down and up
 * inside that window was never sampled at all — which is why only a *held* key
 * ever registered. Every drawing stage pumps this accumulator, so the worst
 * case latency is one waterfall band instead of one whole frame. */
static ui_button_event_t ev_acc;

static void pump_buttons(void) {
    ui_button_event_t e;
    buttons_poll(&e);
    ev_acc.short_press |= e.short_press;
    ev_acc.repeat |= e.repeat;
    ev_acc.long_press |= e.long_press;
    if (e.chord_mute) ev_acc.chord_mute = true;
    if (e.short_press & ((1u << UI_BTN_LEFT) | (1u << UI_BTN_RIGHT))) ev_acc.dir_mod = e.dir_mod;
}

static struct {
    bool valid;
    bool live, muted;
    int dpk, dact, ddet;
    unsigned ch, mhz;
    uint16_t edge;
} top_cache;

static void acquire(void);
static int64_t last_acq_us;

/* Acquisition is serviced from inside the renderer.
 *
 * The panel is slow: one full repaint moves ~250 KB over a 4 MHz bus, i.e. a
 * quarter of a second. When the sweep was only stepped at the top of the UI
 * loop, that quarter second became the step time and a 39-step 5G pass took
 * ten seconds instead of three. Every drawing stage now offers the RF engine a
 * slot, so a pass keeps its nominal 80 ms cadence and the picture simply takes
 * as long as it takes. */
static void maybe_acquire(void) {
    if (usb_active) return;
    int64_t now = esp_timer_get_time();
    if (now - last_acq_us < SWEEP_PERIOD_US) return;
    last_acq_us = now;
    acquire();
}

static void render_top(unsigned w) {
    unsigned top = layout.top_h;
    unsigned ty = (top - FONT7X12_H) / 2;
    /* The status bar only changes when a number does, but repainting it every
     * frame blanked and refilled 30 rows, which reads as a flash along the top
     * edge. Keep the last painted state and skip the whole band when nothing
     * moved. */
    bool live = cfg.mode == UI_MODE_LIVE;
    int dpk = pk_db, dact = act_db, ddet = det_pct;
    if (cfg.mode == UI_MODE_SWEEP && ((ch_seen >> channel) & 1u)) {
        dpk = ch_pk_db[channel];
        dact = ch_act_db[channel];
        ddet = ch_det[channel];
    }
    unsigned shown = usb_active ? sdr_ui_frequency() : band_mhz(channel);
    uint16_t edge0 = live ? COL_GREEN : COL_PEAK;
    if (top_cache.valid && top_cache.live == live && top_cache.muted == muted &&
        top_cache.dpk == dpk && top_cache.dact == dact && top_cache.ddet == ddet &&
        top_cache.ch == channel && top_cache.mhz == shown && top_cache.edge == edge0)
        return;

    /* LIVE changes pk/act/det on every capture, so the cache above misses
     * almost every frame. The repaint is now one atomic bitmap, this throttle
     * only keeps the numbers from twitching at the capture rate. Structural
     * changes (mode/band/mute/channel) still paint immediately. */
    static int64_t last_top_us;
    int64_t now_us = esp_timer_get_time();
    bool structural = !top_cache.valid || top_cache.live != live ||
                      top_cache.muted != muted || top_cache.ch != channel ||
                      top_cache.mhz != shown || top_cache.edge != edge0;
    if (!structural && now_us - last_top_us < 250000) return;
    last_top_us = now_us;

    /* Compose the whole bar into the shared 2-bit mask and send it as ONE
     * bitmap. mask is free here — render_spectrum() clears it before its own
     * pass. Four colours suffice: panel, text, accent, tint. No fill_rect, no
     * text windows and no CS edges between the 30 rows, so the panel scan can
     * no longer cut through a half-rebuilt status bar. */
    unsigned stride = (w + 3) / 4;
    memset(mask, 0, stride * top);
    uint16_t tint = live ? COL_TINT_G : COL_TINT_O;
    uint16_t edge = live ? COL_GREEN : COL_PEAK;
    uint16_t pal[4] = {COL_PANEL, COL_TEXT, edge, tint};

    const char *band = cfg.band == UI_BAND_5G ? "5G" : "2.4G";
    mask_text_big(4, ty, band, 1);

    unsigned bx = 4 + text_w_big(band) + 5;
    unsigned bw = 46, bh = FONT7X12_H + 8, by = (top - bh) / 2;
    mask_rect(bx, by, bw, bh, 3);
    mask_rect(bx, by, bw, 1, 2);
    mask_rect(bx, by + bh - 1, bw, 1, 2);
    mask_rect(bx, by, 1, bh, 2);
    mask_rect(bx + bw - 1, by, 1, bh, 2);
    mask_put(bx, by, 0);
    mask_put(bx + bw - 1, by, 0);
    mask_put(bx, by + bh - 1, 0);
    mask_put(bx + bw - 1, by + bh - 1, 0);
    const char *mode = live ? "LIVE" : "SWEEP";
    mask_text_big(bx + (bw - text_w_big(mode)) / 2, ty, mode, 2);

    char line[40];
    if (w >= 300)
        snprintf(line, sizeof(line), "PK%d A%d D%d%%", dpk, dact, ddet);
    else
        snprintf(line, sizeof(line), "P%d D%d%%", dpk, ddet);
    mask_text_big(bx + bw + 6, ty, line, 1);

    if (w >= 300)
        snprintf(line, sizeof(line), "ch%u %uM", band_number(channel), shown);
    else
        snprintf(line, sizeof(line), "%u/%uM", band_number(channel), shown);
    unsigned tw = text_w_big(line);
    unsigned tx = (w > 4 + tw) ? w - 4 - tw : 0;
    mask_text_big(tx, ty, line, 1);
    if (muted) mask_mute_icon(tx > 12 ? tx - 6 : w - 2, top / 2);

    lcd_draw_bitmap2(0, 0, w, top, mask, pal);

    top_cache.valid = true;
    top_cache.live = live;
    top_cache.muted = muted;
    top_cache.dpk = dpk;
    top_cache.dact = dact;
    top_cache.ddet = ddet;
    top_cache.ch = channel;
    top_cache.mhz = shown;
    top_cache.edge = edge0;
}


static void render_spectrum(unsigned w) {
    unsigned h = layout.spec_h;
    unsigned stride = (w + 3) / 4;
    /* No background fill first: the bitmap below covers every pixel of the
     * region (index 0 is the background colour), and clearing before painting
     * is exactly what made the curve blink once per frame — the area was
     * blanked, then repainted row by row over ~120 ms. */
    memset(mask, 0, stride * h);

    int range = view_top_db - view_bot_db;
    if (range < 5) range = 5;

    for (int db = -140; db <= 20; db += 20) {
        if (db < view_bot_db || db > view_top_db) continue;
        int y = (int)h - (db - view_bot_db) * (int)h / range;
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
            int mean = level_of_code(disp_code(x, w), h);
            int hold = level_of_code(disp_hold(x, w), h);
            if (hold < mean) hold = mean;
            int y = (int)h - 1 - mean;
            if (y < 0) y = 0;
            int ph = (int)h - 1 - hold;
            if (ph < 0) ph = 0;
            /* Index 1, not 4: mask_put() keeps only two bits, so the old 4 was
             * truncated to 0 and turned the peak band into a black eraser. The
             * grid colour is the intended near-black wash. */
            for (unsigned yy = (unsigned)ph; yy < h; yy++) mask_put(x, yy, 1);
            unsigned lo = (unsigned)(prev < 0 ? y : (prev < y ? prev : y));
            unsigned hi = (unsigned)(prev < 0 ? y : (prev > y ? prev : y));
            for (unsigned yy = lo; yy <= hi; yy++) mask_put(x, yy, 2);
            prev = y;
            mask_put(x, (unsigned)ph, 3);
        }
    }
    char line[32];
    for (int db = -140; db <= 20; db += 20) {
        if (db < view_bot_db || db > view_top_db) continue;
        int y = (int)h - (db - view_bot_db) * (int)h / range;
        if (y <= 9 || y >= (int)h - 11) continue;
        snprintf(line, sizeof(line), "%d", db);
        mask_text(3, (unsigned)y - FONT5X7_H - 1, line, 1);
    }

    unsigned shown = usb_active ? sdr_ui_frequency() : band_mhz(channel);
    unsigned lo_mhz = shown - 10, hi_mhz = shown + 10;
    if (cfg.mode == UI_MODE_SWEEP || usb_active) {
        lo_mhz = band_lo();
        hi_mhz = band_hi();
    }
    snprintf(line, sizeof(line), "%u", lo_mhz);
    mask_text(4, h - FONT5X7_H - 3, line, 1);
    snprintf(line, sizeof(line), "%u", hi_mhz);
    unsigned tw = text_w(line);
    mask_text(w - 4 - tw, h - FONT5X7_H - 3, line, 1);

    /* Sweep (and host-linked views) show the whole band, so outline the 20 MHz
     * window belonging to the selected channel — the box from the approved HTML
     * mock. The 2-bit mask has no spare colour for its translucent fill, so only
     * the two vertical edges and the bright top/bottom rules are drawn, in the
     * amber peak colour (the curve already owns cyan). Painted before the
     * callout so a channel at the far left cannot obscure the label. strip_x()
     * is shared with the channel strip, which keeps this box aligned with the
     * cursor underneath. */
    if (cfg.mode == UI_MODE_SWEEP || usb_active) {
        unsigned x0 = strip_x(shown >= 10 ? shown - 10 : 0, w);
        unsigned x1 = strip_x(shown + 10, w);
        if (x0 >= w) x0 = w - 1;
        if (x1 >= w) x1 = w - 1;
        if (x1 < x0) { unsigned t = x0; x0 = x1; x1 = t; }
        if (x1 - x0 < 2) x1 = (x0 + 2 < w) ? x0 + 2 : w - 1;
        mask_rect(x0, 0, 1, h, 3);
        mask_rect(x1, 0, 1, h, 3);
        mask_rect(x0, 0, x1 - x0 + 1, 1, 3);
        mask_rect(x0, h - 1, x1 - x0 + 1, 1, 3);
    }

    if (cfg.mode == UI_MODE_LIVE) {
        unsigned cx = w / 2;
        for (unsigned y = 0; y < h; y += 5) {
            unsigned seg = h - y < 3 ? h - y : 3;
            mask_rect(cx, y, 1, seg, 2);
        }
        mask_text(4, 4, "LIVE 20M", 2);
    } else {
        char lab[40];
        int cdet = ((ch_seen >> channel) & 1u) ? ch_det[channel] : det_pct;
        if (cfg.mode == UI_MODE_SWEEP && !usb_active)
            snprintf(lab, sizeof(lab), "ch%u %u/%u %d%%", band_number(channel),
                     (unsigned)scan_index + 1, sweep_steps(), cdet);
        else
            snprintf(lab, sizeof(lab), "ch%u %uM  %d%%", band_number(channel), shown, cdet);
        unsigned lw = text_w(lab) + 10;
        /* Box, border and text all live in the bitmap: index 0 clears the graph
         * underneath, 1 draws the frame, 3 the amber legend. */
        mask_rect(3, 5, lw, 13, 0);
        mask_rect(3, 5, lw, 1, 1);
        mask_rect(3, 17, lw, 1, 1);
        mask_rect(3, 5, 1, 13, 1);
        mask_rect(lw + 2, 5, 1, 13, 1);
        mask_text(8, 8, lab, 3);
    }

    /* One window, one continuous bitmap. It used to be split into two halves
     * with a capture slot in between (the call took 120 ms at 4 MHz, longer than
     * the 80 ms sweep step); that gap left a visible horizontal seam right
     * through the middle of the trace. At 8 MHz the whole bitmap takes ~69 ms,
     * so it fits inside a step and the seam is gone. */
    pump_buttons();
    lcd_draw_bitmap2(0, layout.spec_y, w, h, mask, spec_palette);
}

static void render_waterfall(unsigned w) {
    unsigned rows = layout.wf_h;
    if (!rows) return;
    /* One window and one continuous RAMWR stream for the whole strip. Re-opening
     * a one-row window per line cost a command burst and a CS edge each time, and
     * let the panel's own scan interleave with the write — in LIVE that read as a
     * seam walking down the screen. The ring index is frozen for this frame, and
     * no capture runs while it is being read, so the rows cannot shift under us. */
    unsigned base = wf_next;
    int range = view_top_db - view_bot_db;
    if (range < 5) range = 5;
    lcd_set_window(0, layout.wf_y, w, rows);
    for (unsigned r = 0; r < rows; r++) {
        unsigned src_r = rows > WF_ROWS_MAX ? r * WF_ROWS_MAX / rows : r;
        if (src_r >= WF_ROWS_MAX) src_r = WF_ROWS_MAX - 1;
        const uint8_t *src = wf + ((base + src_r) % WF_ROWS_MAX) * WF_STRIDE;
        for (unsigned x = 0; x < w; x++) {
            int db = (int)(src[x] / 2) - 84;
            int idx = (db - view_bot_db) * 255 / range;
            if (idx < 0) idx = 0;
            if (idx > 255) idx = 255;
            wf_line[x] = wf_palette[idx];
        }
        lcd_pixels_cont(wf_line, w);
        if ((r & 7) == 7) {
            pump_buttons();
            /* Capture slot every 8 rows: that is roughly every 5 ms of bus time,
             * denser than the old 16-row grid so the sweep is limited by the
             * radio (1-2 ms) rather than by how rarely the renderer looks up. */
            maybe_acquire();
        }
    }
    lcd_pixels_end();
}

/* The strip used to be blanked and rebuilt in full on every frame, which read
 * as a constant flicker along the bottom edge. Bars are now repainted one slot
 * at a time and the channel numbers live in their own row, which is painted
 * once per band — nothing can erase them afterwards. */
static struct {
    bool valid;
    unsigned w, count, selected;
    bool usb;
    uint8_t band;
    uint8_t det[36], act[36], pk[36];
    uint8_t gdet[GAP_COUNT], gact[GAP_COUNT], gpk[GAP_COUNT];
} strip_cache;

static unsigned strip_selected(unsigned count) {
    unsigned w = lcd_width();
    unsigned sel = usb_active ? (unsigned)((sel_x * (int)count) / (int)w) : channel;
    return sel >= count ? count - 1 : sel;
}

/* Bars sit on the frequency axis, not on an even grid, so every slot lands
 * directly under the slice of trace it was measured on, and the outermost bars
 * reach the panel edges (the old (w-8)/n cells stopped short of the right
 * edge). 2.4 GHz bypasses this and uses equal cells — see strip_slot(). */
static unsigned strip_x(unsigned mhz, unsigned w) {
    unsigned lo = band_lo(), span = band_hi() - band_lo();
    if (span == 0 || !w) return 0;
    if (mhz < lo) mhz = lo;
    unsigned x = (mhz - lo) * w / span;
    return x > w ? w : x;
}

#define STRIP_LABEL_Y (layout.strip_y + 2)
#define STRIP_DET_Y   (layout.strip_y + 11)
#define STRIP_ACT_Y   (layout.strip_y + 20)
#define STRIP_BAR_H   7

/* 5 GHz channels never overlap (all 20 MHz apart), so a bar can sit on the
 * frequency axis and still line up with the trace above it.
 *
 * 2.4 GHz is the opposite: channels are 5 MHz apart but 20 MHz wide, so ch1 and
 * ch2 overlap by 15 MHz. Mapping those blocks onto the axis made every bar 80 px
 * wide and 20 px apart — thirteen bars drawn on top of each other, which is the
 * "mess" the strip showed in 2.4G sweep. There is no layout that both fills the
 * axis and avoids overlap, so 2.4G uses equal cells: one column pair per
 * channel, spread over the full width. */
static void strip_slot(unsigned i, unsigned w, unsigned *x0, unsigned *x1) {
    if (cfg.band == UI_BAND_24G) {
        unsigned count = band_count();
        if (!count || i >= count) {
            *x0 = 0;
            *x1 = 0;
            return;
        }
        *x0 = i * w / count;
        *x1 = (i + 1) * w / count;
        if (*x1 <= *x0 + 1) *x1 = *x0 + 2;
        return;
    }
    unsigned f = band_mhz(i);
    *x0 = strip_x(f > 10 ? f - 10 : 0, w);
    *x1 = strip_x(f + 10, w);
    if (*x1 <= *x0 + 1) *x1 = *x0 + 2;
}

static void strip_draw_bar(unsigned i, bool sel) {
    unsigned w = lcd_width(), x0, x1;
    if (i >= band_count()) return;
    strip_slot(i, w, &x0, &x1);
    /* 1 px gap between neighbouring channels, but the outermost block runs to
     * the panel edge so the strip looks flush on both sides. */
    unsigned slot = (x1 >= w) ? (x1 - x0) : (x1 > x0 + 1 ? x1 - x0 - 1 : 1);
    unsigned det = ch_det[i] > 100 ? 100 : ch_det[i];
    unsigned act = ch_act[i] > 100 ? 100 : ch_act[i];

    lcd_fill_rect(x0, STRIP_DET_Y, slot, STRIP_BAR_H, sel ? COL_TINT_C : 0x18C3);
    if (det > 2) {
        unsigned bw = slot * det / 100;
        uint16_t dc = ch_pk[i] > 70 ? COL_RED : ch_pk[i] > 40 ? COL_PEAK : COL_GREEN;
        lcd_fill_rect(x0, STRIP_DET_Y, bw ? bw : 1, STRIP_BAR_H, dc);
    }
    lcd_fill_rect(x0, STRIP_ACT_Y, slot, STRIP_BAR_H, sel ? COL_TINT_C : 0x1082);
    if (act > 2) {
        unsigned bw = slot * act / 100;
        lcd_fill_rect(x0, STRIP_ACT_Y, bw ? bw : 1, STRIP_BAR_H, COL_USB);
    }
}

/* Special stretches (RADAR / SAT / ISM) get the same two bars as the channels.
 * The label row above them carries the band's purpose instead of a channel
 * number, which is why they are drawn here and not by strip_draw_bar(). */
static void strip_draw_gap_bars(void) {
    unsigned w = lcd_width();
    for (unsigned g = 0; g < gap_count(); g++) {
        unsigned x0 = strip_x(gap_lo(g), w), x1 = strip_x(gap_hi(g), w);
        if (x1 <= x0) continue;
        unsigned slot = x1 - x0;
        unsigned det = gap_det[g] > 100 ? 100 : gap_det[g];
        unsigned act = gap_act[g] > 100 ? 100 : gap_act[g];
        lcd_fill_rect(x0, STRIP_DET_Y, slot, STRIP_BAR_H, 0x18A1);
        if (det > 2) {
            unsigned bw = slot * det / 100;
            uint16_t dc = gap_pk[g] > 70 ? COL_RED : gap_pk[g] > 40 ? COL_PEAK : COL_GREEN;
            lcd_fill_rect(x0, STRIP_DET_Y, bw ? bw : 1, STRIP_BAR_H, dc);
        }
        lcd_fill_rect(x0, STRIP_ACT_Y, slot, STRIP_BAR_H, 0x10A1);
        if (act > 2) {
            unsigned bw = slot * act / 100;
            lcd_fill_rect(x0, STRIP_ACT_Y, bw ? bw : 1, STRIP_BAR_H, COL_USB);
        }
    }
}

static void strip_draw_labels(unsigned w) {
    unsigned count = band_count();
    unsigned last = 0;
    bool first = true;
    char label[8];
    for (unsigned i = 0; i < count; i++) {
        unsigned x0, x1;
        strip_slot(i, w, &x0, &x1);
        snprintf(label, sizeof(label), "%u", band_number(i));
        unsigned tw = text_w(label);
        int lx = (int)((x0 + x1) / 2) - (int)(tw / 2);
        if (lx < 1) lx = 1;
        if (lx + (int)tw > (int)w - 1) lx = (int)w - 1 - (int)tw;
        bool last_one = i + 1 == count;
        if (!first && !last_one && (unsigned)lx < last + 4) continue;
        ui_text((unsigned)lx, STRIP_LABEL_Y, label, COL_DIM, COL_STRIP, false);
        last = (unsigned)(lx + (int)tw);
        first = false;
    }
    /* Purpose labels for the special stretches. A 5 GHz stretch is often only a
     * few pixels wide — 20 MHz of 725 MHz is ~9 px, and "RADAR" needs 29 px — so
     * the word is replaced by its initial when the full one cannot fit. A 2 px
     * sliver (the 5730-5735 ISM gap) cannot hold even one glyph and keeps bars
     * only. */
    for (unsigned g = 0; g < gap_count(); g++) {
        unsigned x0 = strip_x(gap_lo(g), w), x1 = strip_x(gap_hi(g), w);
        if (x1 <= x0) continue;
        char initial[2] = {gap_tag(g)[0], 0};
        const char *tag = gap_tag(g);
        unsigned tw = text_w(tag);
        if (tw + 3 > x1 - x0) {
            tag = initial;
            tw = text_w(tag);
            if (tw + 2 > x1 - x0) continue;
        }
        int lx = (int)((x0 + x1) / 2) - (int)(tw / 2);
        if (lx < 1) lx = 1;
        if (lx + (int)tw > (int)w - 1) lx = (int)w - 1 - (int)tw;
        ui_text((unsigned)lx, STRIP_LABEL_Y, tag, COL_PEAK, COL_STRIP, false);
    }
}

/* Cursor: a short accent underline under the tuned 20 MHz block, in the empty
 * row at the bottom of the strip. Erasing it is one flat fill, so moving the
 * cursor never disturbs the labels above. */
static void strip_draw_cursor(unsigned idx, bool on) {
    unsigned w = lcd_width(), x0, x1;
    if (idx >= band_count()) return;
    strip_slot(idx, w, &x0, &x1);
    unsigned slot = (x1 >= w) ? (x1 - x0) : (x1 > x0 + 1 ? x1 - x0 - 1 : 1);
    lcd_fill_rect(x0, layout.strip_y + layout.strip_h - 3, slot, 2,
                  on ? COL_ACCENT : COL_STRIP);
}

static void render_strip(unsigned w) {
    unsigned count = band_count();
    if (!layout.strip_h || !count) {
        strip_cache.valid = false;
        return;
    }
    unsigned selected = strip_selected(count);
    bool full = !strip_cache.valid || strip_cache.w != w || strip_cache.count != count ||
                strip_cache.usb != usb_active || strip_cache.band != cfg.band;
    if (full) {
        lcd_fill_rect(0, layout.strip_y, w, layout.strip_h, COL_STRIP);
        lcd_fill_rect(0, layout.strip_y, w, 1, COL_BG);
        strip_draw_gap_bars();
        for (unsigned i = 0; i < count; i++) strip_draw_bar(i, false);
        strip_draw_labels(w);
        strip_draw_cursor(selected, true);
        if (usb_active) {
            lcd_fill_rect(w / 2 - 18, layout.strip_y + 2, 36, layout.strip_h - 4, COL_STRIP);
            ui_text(w / 2 - 16, layout.strip_y + (layout.strip_h - FONT5X7_H) / 2, "USB", COL_USB,
                    COL_STRIP, false);
        }
    } else {
        if (strip_cache.selected != selected) {
            strip_draw_cursor(strip_cache.selected, false);
            strip_draw_bar(strip_cache.selected, false);
            strip_draw_cursor(selected, true);
        }
        strip_draw_bar(selected, true);
        for (unsigned i = 0; i < count; i++) {
            if (strip_cache.det[i] != ch_det[i] || strip_cache.act[i] != ch_act[i] ||
                strip_cache.pk[i] != ch_pk[i])
                strip_draw_bar(i, i == selected);
        }
        for (unsigned g = 0; g < gap_count(); g++)
            if (strip_cache.gdet[g] != gap_det[g] || strip_cache.gact[g] != gap_act[g] ||
                strip_cache.gpk[g] != gap_pk[g]) {
                strip_draw_gap_bars();
                break;
            }
    }
    strip_cache.valid = true;
    strip_cache.w = w;
    strip_cache.count = count;
    strip_cache.selected = selected;
    strip_cache.usb = usb_active;
    strip_cache.band = cfg.band;
    for (unsigned i = 0; i < count && i < 36; i++) {
        strip_cache.det[i] = ch_det[i];
        strip_cache.act[i] = ch_act[i];
        strip_cache.pk[i] = ch_pk[i];
    }
    for (unsigned g = 0; g < gap_count(); g++) {
        strip_cache.gdet[g] = gap_det[g];
        strip_cache.gact[g] = gap_act[g];
        strip_cache.gpk[g] = gap_pk[g];
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
    int64_t t_frame = esp_timer_get_time();
    unsigned w = lcd_width();
    layout_build();
    pump_buttons();
    if (full_clear) {
        lcd_fill(COL_BG);
        top_cache.valid = false;
        strip_cache.valid = false;
        full_clear = false;
    }
    render_top(w);
    pump_buttons();
    maybe_acquire();
    render_spectrum(w);
    pump_buttons();
    maybe_acquire();
    render_waterfall(w);
    maybe_acquire();
    render_strip(w);
    render_overlays(w);
    maybe_acquire();
    last_frame_ms = (unsigned)((esp_timer_get_time() - t_frame) / 1000);
}

/* Publish the channel bars when a pass ends, using the phone app's rules: the
 * floor is the 20th percentile of the plotted band, each channel takes the peak
 * hold within +/-8 MHz of its centre and counts as detected 12 dB above the floor. */
static void publish_bars(void) {
    unsigned w = lcd_width(), count = band_count();
    if (!count) return;
    int hist[64];
    unsigned n = 0;
    for (unsigned k = 0; k < 64; k++) {
        unsigned x = (k * (w - 1)) / 63;
        if (cfg.mode == UI_MODE_SWEEP || usb_active) {
            if (!col_seen_bit(x)) continue;
            hist[n++] = (int)band_codes[x] / 2 - 84;
        } else {
            hist[n++] = (int)lerp8(codes, FFT_BINS, x, w) / 2 - 84;
        }
    }
    if (!n) return;
    for (unsigned i = 1; i < n; i++) {
        int v = hist[i], j = (int)i;
        while (j > 0 && hist[j - 1] > v) {
            hist[j] = hist[j - 1];
            j--;
        }
        hist[j] = v;
    }
    int floor_db = hist[n / 5];
    floor_db_seen = floor_db; /* diagnostics only: the dB axis no longer follows it */
    sweeps++;
    for (unsigned c = 0; c < count && c < 36; c++) {
        unsigned centre = band_mhz(c);
        unsigned pk = 0;
        for (unsigned f = (centre >= 8 ? centre - 8 : 0); f <= centre + 8; f++) {
            int col = column_of(f, w);
            if (col < 0) continue;
            if (band_peak[col] > pk) pk = band_peak[col];
        }
        int pk_db_c = (int)pk / 2 - 84;
        int act = pk_db_c - floor_db;
        if (act < 0) act = 0;
        if (act >= 12) ch_hits[c]++;
        ch_pk[c] = (uint8_t)(act > 100 ? 100 : act);
        ch_act[c] = (uint8_t)(act > 60 ? 100 : act * 100 / 60);
        ch_det[c] = (uint8_t)(sweeps ? 100u * ch_hits[c] / sweeps : 0);
        ch_pk_db[c] = (int8_t)pk_db_c;
        ch_act_db[c] = (int8_t)act;
        ch_seen |= (uint64_t)1 << c;
    }
    /* The stretches without a channel number use the same yardstick: peak inside
     * the range, measured against the same floor, 12 dB up counts as detected. */
    for (unsigned g = 0; g < gap_count(); g++) {
        unsigned x0 = strip_x(gap_lo(g), w), x1 = strip_x(gap_hi(g), w);
        unsigned pk = 0;
        for (unsigned x = x0; x < x1 && x < w; x++)
            if (band_peak[x] > pk) pk = band_peak[x];
        int pk_db_c = (int)pk / 2 - 84;
        int act = pk_db_c - floor_db;
        if (act < 0) act = 0;
        if (act >= 12) gap_hits[g]++;
        gap_pk[g] = (uint8_t)(act > 100 ? 100 : act);
        gap_act[g] = (uint8_t)(act > 60 ? 100 : act * 100 / 60);
        gap_det[g] = (uint8_t)(sweeps ? 100u * gap_hits[g] / sweeps : 0);
    }
}

static void waterfall_push(void) {
    unsigned w = lcd_width();
    uint8_t *row = wf + wf_next * WF_STRIDE;
    if (w > WF_STRIDE) w = WF_STRIDE;
    if (cfg.mode == UI_MODE_SWEEP || usb_active)
        memcpy(row, band_codes, w);
    else
        for (unsigned x = 0; x < w; x++) row[x] = lerp8(codes, FFT_BINS, x, w);
    /* The ring is always WF_ROWS_MAX deep. Layout height is only a display
     * size now: indexing by it wrote past the buffer (84 rows of history into a
     * 64-row ring) and corrupted the heap, which is what rebooted the chip on
     * every mode change. */
    wf_next = (wf_next + 1) % WF_ROWS_MAX;
}

static void analyse(void) {
    /* Counting sort over the 8-bit codes. The old insertion sort was O(n^2) on
     * 256 entries — up to ~32k compares per step — and this runs on every capture.
     * hist lives on the stack: a static copy pushed BSS into the C5 RF dump
     * window at 0x40830000 and the linker rejects that outright. Counts saturate
     * at 255, which cannot happen with 256 bins unless every bin is identical. */
    uint8_t hist[256];
    memset(hist, 0, sizeof(hist));
    unsigned sum = 0, pk = 0;
    for (unsigned j = 0; j < FFT_BINS; j++) {
        if (hist[codes[j]] < 255) hist[codes[j]]++;
        sum += codes[j];
        if (codes[j] > pk) pk = codes[j];
    }
    unsigned noise = 0, acc = 0;
    const unsigned target = FFT_BINS / 4;
    for (unsigned i = 0; i < 256; i++) {
        acc += hist[i];
        if (acc > target) {
            noise = i;
            break;
        }
    }
    unsigned det = 0;
    for (unsigned j = 0; j < FFT_BINS; j++)
        if (codes[j] > noise + 16) det++;

    pk_db = (int)(pk / 2) - 84;
    act_db = ((int)(sum / FFT_BINS) - (int)noise) / 2;
    det_pct = (int)(det * 100 / FFT_BINS);
}

static void stats_store(unsigned i) {
    if (i >= 36) return;
    int lv = (pk_db - (view_bot_db + 4)) * 100 / 50;
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

static void acquire(void) {
    int64_t t_start = esp_timer_get_time();
    unsigned w = lcd_width();
    bool sweep = cfg.mode == UI_MODE_SWEEP;
    unsigned mhz = sweep ? sweep_lo(scan_index) : band_mhz(channel);
    unsigned elapsed = 0;
    if (!sdr_ui_spectrum(mhz, FFT_BINS, sweep ? RATE_SWEEP : RATE_CODE, codes, &elapsed)) {
        acq_fail++;
        return;
    }
    acq_ok++;
    have_data = true;

    if (sweep) {
        /* Keep only the +/-10..20 MHz ring of an 80 MS/s capture, like the app: the
         * DC region around the LO is skipped and a neighbouring step covers it.
         *
         * Averaging happens in the code (dB) domain. The old version accumulated
         * linear power with one powf() per bin and then ran log10f() over all 320
         * columns, i.e. 576 libm calls per step; measured on hardware a step cost
         * 36 ms while the RF part is only 1-2 ms, so those calls were the sweep's
         * real limit. Averaging codes needs neither function, and on noise the two
         * agree within a fraction of a dB (isolated peaks come out ~1 dB high). */
        const float df = 80.0f / (float)FFT_BINS;
        unsigned col_lo = w, col_hi = 0;
        for (unsigned j = 0; j < FFT_BINS; j++) {
            float rel = ((float)j - (float)FFT_BINS / 2.0f) * df;
            float a = rel < 0 ? -rel : rel;
            if (a < 10.0f || a > 20.0f) continue;
            int col = column_of_f((float)mhz + rel, w);
            if (col < 0) continue;
            unsigned uc = (unsigned)col;
            if (uc < col_lo) col_lo = uc;
            if (uc > col_hi) col_hi = uc;
            col_seen_set(uc);
            if (spec_sum) spec_sum[uc] += (float)codes[j];
            if (spec_cnt) spec_cnt[uc]++;
            if (codes[j] >= band_peak[uc]) band_peak[uc] = codes[j];
        }
        /* Only the columns this capture touched can have changed. Recomputing all
         * 320 of them every step was most of the remaining cost. */
        if (spec_sum && spec_cnt && col_lo <= col_hi)
            for (unsigned x = col_lo; x <= col_hi; x++) {
                if (!spec_cnt[x]) continue;
                band_codes[x] = (uint8_t)(spec_sum[x] / (float)spec_cnt[x] + 0.5f);
            }
        sweep_step_count++;
        if (++scan_index >= sweep_steps()) {
            /* pass finished: drop PEAK_DECAY_DB (2 dB = 4 codes) from the hold and
             * publish the bars for the sweep that just ended */
            scan_index = 0;
            for (unsigned x = 0; x < w; x++)
                band_peak[x] = band_peak[x] > 4 ? (uint8_t)(band_peak[x] - 4) : 0;
            if (spec_sum) memset(spec_sum, 0, 320 * sizeof(float));
            if (spec_cnt) memset(spec_cnt, 0, 320 * sizeof(uint16_t));
            publish_bars();
        }
    } else {
        if (channel != last_tuned) {
            if (win_peak) memset(win_peak, 0, FFT_BINS);
            last_tuned = channel;
        }
        if (win_peak)
            for (unsigned i = 0; i < FFT_BINS; i++) {
                if (codes[i] >= win_peak[i])
                    win_peak[i] = codes[i];
                else if (win_peak[i])
                    win_peak[i]--;
            }
        publish_bars();
    }
    analyse();
    waterfall_push();
    /* Whole-step cost (RF + spectrum accumulation + sort), not just the RF part. */
    last_acq_ms = (unsigned)((esp_timer_get_time() - t_start) / 1000);
}

static void change_channel(int direction) {
    unsigned count = band_count();
    channel = (uint8_t)((channel + (direction > 0 ? 1 : count - 1)) % count);
    last_step = direction > 0 ? 1 : -1;
    buzzer_play(UI_BUZZ_CHANNEL);
}

static void adjust_limit(bool top, int direction) {
    /* A chord only edits the pair for the band + mode on screen. Tuning LIVE
     * on 5G cannot move 2.4G, SWEEP, or any other combination. */
    unsigned b = scale_band();
    bool live = cfg.mode == UI_MODE_LIVE;
    int8_t *hi = live ? &cfg.live_top_db[b] : &cfg.top_db[b];
    int8_t *lo = live ? &cfg.live_bottom_db[b] : &cfg.bottom_db[b];
    int8_t *limit = top ? hi : lo;
    int value = *limit + (direction > 0 ? 5 : -5);
    if (top) {
        if (value > -10) value = -10;
        if (value < -80) value = -80;
        if (value < *lo + 30) value = *lo + 30;
    } else {
        if (value < -125) value = -125;
        if (value > -55) value = -55;
        if (value > *hi - 30) value = *hi - 30;
    }
    *limit = (int8_t)value;
    apply_stored_scale();
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
    memset(col_seen, 0, sizeof(col_seen));
    memset(ch_hits, 0, sizeof(ch_hits));
    memset(gap_pk, 0, sizeof(gap_pk));
    memset(gap_act, 0, sizeof(gap_act));
    memset(gap_det, 0, sizeof(gap_det));
    memset(gap_hits, 0, sizeof(gap_hits));
    if (win_peak) memset(win_peak, 0, FFT_BINS);
    sweeps = 0;
    /* Keep each mode's own limits: switching band must not undo a manual range
     * set with a chord, nor leave the axis on the other mode's values. */
    apply_stored_scale();
    ch_seen = 0;
    have_data = false;
    ui_settings_save(&cfg);
    buzzer_play(UI_BUZZ_BAND);
    full_clear = true;
}

static void toggle_mode(void) {
    cfg.mode = cfg.mode == UI_MODE_SWEEP ? UI_MODE_LIVE : UI_MODE_SWEEP;
    if (cfg.mode == UI_MODE_SWEEP) channel = (uint8_t)middle_index();
    /* Snap to the incoming mode's own limits so the first frame after the
     * switch never shows the other mode's dB window. */
    apply_stored_scale();
    layout_build();
    scan_index = 0;
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
    apply_stored_scale();
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
    memset(gap_pk, 0, sizeof(gap_pk));
    memset(gap_act, 0, sizeof(gap_act));
    memset(gap_det, 0, sizeof(gap_det));
    memset(gap_hits, 0, sizeof(gap_hits));
    memset(wf, 0, WF_STRIDE * WF_ROWS_MAX);
    have_data = false;
    ui_settings_clear();
    buzzer_play(UI_BUZZ_RESET);
    full_clear = true;
}

static void toggle_mute(void) {
    muted = !muted;
    cfg.mute = muted ? 1 : 0;
    /* Both directions go through here, so muting and un-muting are both written
     * to NVS and both survive a reboot. */
    ui_settings_save(&cfg);
    buzzer_enable(!muted);
    if (!muted) buzzer_play(UI_BUZZ_CHANNEL);
    trace("MUTE %d buzz=%d\n", (int)muted, (int)(cfg.buzz != 0 && !muted));
}

static void handle_events(const ui_button_event_t *event) {
    uint16_t left = (uint16_t)(1u << UI_BTN_LEFT), right = (uint16_t)(1u << UI_BTN_RIGHT);
    if (event->short_press || event->repeat || event->long_press || event->chord_mute)
        trace("UI ev short=%04x rep=%04x long=%04x chord=%d\n", event->short_press, event->repeat,
              event->long_press, (int)event->chord_mute);
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
    int64_t last_render = 0, last_tick = 0;
    trace("UI task running\n");
    for (;;) {
        int64_t now = esp_timer_get_time();
        bool active = sdr_ui_host_active();
        if (active != usb_active) {
            usb_active = active;
            if (active) full_clear = false;
        }
        pump_buttons();
        countdown_poll(now);
        if (ev_acc.short_press || ev_acc.repeat || ev_acc.long_press || ev_acc.chord_mute) {
            handle_events(&ev_acc);
            memset(&ev_acc, 0, sizeof(ev_acc));
        }
        maybe_acquire();
        unsigned period = usb_active ? HOST_PERIOD_US : RENDER_PERIOD_US;
        if (now - last_render >= period) {
            last_render = now;
            if (!usb_active) {
                sel_x = column_of(band_mhz(channel), lcd_width());
                if (sel_x < 0) sel_x = 0;
            }
            render();
            /* Anything pressed while the frame was being painted is handled at
             * the top of the next pass, so it still lands within ~10 ms. */
        }
        if (now - last_tick >= 2000000) {
            /* Report steps per interval inside the firmware: the UART drops lines
             * at 2 Mbaud, so a host-side diff of scan= is not trustworthy. */
            static unsigned prev_steps;
            static int64_t prev_tick;
            unsigned steps = sweep_step_count - prev_steps;
            unsigned dms = (unsigned)((now - prev_tick) / 1000);
            prev_steps = sweep_step_count;
            prev_tick = now;
            last_tick = now;
            trace("UI tick host=%d ch=%u scan=%u/%u steps=%u in %ums pk=%d act=%d fl=%d "
                  "acq=%ums frame=%ums ok=%u fail=%u free=%u\n",
                  (int)usb_active, (unsigned)channel, (unsigned)scan_index + 1, sweep_steps(),
                  steps, dms,
                  pk_db, act_db, floor_db_seen, last_acq_ms, last_frame_ms, acq_ok, acq_fail,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void palette_init(void) {
    /* Cool-to-hot gradient from the approved design: navy, teal, green, amber, red.
     * The first stop is pure black so the noise floor of the waterfall melts into
     * the background instead of painting the whole strip navy. */
    static const uint8_t stops[8][3] = {
        {0, 0, 0},      {8, 30, 52},    {10, 88, 118},  {18, 179, 168},
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
    trace("UI enter\n");
    if (!lcd_init()) {
        trace("UI lcd_init FAILED\n");
        return;
    }
    trace("UI lcd ok\n");
    mask = heap_caps_malloc(MASK_BYTES, MALLOC_CAP_8BIT);
    spec_sum = heap_caps_malloc(320 * sizeof(float), MALLOC_CAP_8BIT);
    win_peak = heap_caps_malloc(FFT_BINS, MALLOC_CAP_8BIT);
    spec_cnt = heap_caps_malloc(320 * sizeof(uint16_t), MALLOC_CAP_8BIT);
    wf = heap_caps_malloc(WF_STRIDE * WF_ROWS_MAX, MALLOC_CAP_8BIT);
    wf_line = heap_caps_malloc(320 * sizeof(uint16_t), MALLOC_CAP_8BIT);
    text_buf = heap_caps_malloc(TEXT_PIXELS * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (!mask || !wf || !wf_line || !text_buf) {
        trace("UI alloc FAILED mask=%d wf=%d line=%d text=%d\n", mask != NULL, wf != NULL,
              wf_line != NULL, text_buf != NULL);
        return;
    }
    memset(wf, 0, WF_STRIDE * WF_ROWS_MAX);
    palette_init();

    ui_settings_load(&cfg);
    lcd_set_rotation(cfg.landscape != 0);
    layout_build();
    /* Start on the stored limits for the boot mode, not on the compile-time
     * -30/-100 defaults that a SWEEP auto-range would then walk away from. */
    apply_stored_scale();

    buttons_init();
    buzzer_init();
    buzzer_enable(cfg.buzz != 0 && !cfg.mute);
    buzzer_play(UI_BUZZ_BOOT);

    muted = cfg.mute != 0;
    channel = (uint8_t)middle_index();
    scan_index = 0;
    BaseType_t created = xTaskCreate(ui_task, "local_ui", 6144, NULL, 3, NULL);
    if (created != pdPASS)
        created = xTaskCreate(ui_task, "local_ui", 4096, NULL, 3, NULL);
    trace("UI ready task=%d buzz=%d mute=%d heap=%u\n", (int)created, (int)cfg.buzz, (int)cfg.mute,
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
}
