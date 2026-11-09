/* Non-blocking buzzer on a plain GPIO. Patterns are millisecond on/off pairs;
 * a higher-priority pattern preempts the one currently sounding. */
#include "buzzer.h"
#include "board_c5_lcd.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

typedef struct {
    const uint16_t *pattern; /* 0 terminates */
    uint8_t priority;
} buzz_pattern_t;

static const uint16_t pat_boot[] = {60, 60, 60, 0};
static const uint16_t pat_band[] = {120, 0};
static const uint16_t pat_mode[] = {40, 40, 40, 0};
static const uint16_t pat_channel[] = {20, 0};
static const uint16_t pat_top[] = {30, 0};
static const uint16_t pat_bottom[] = {30, 30, 30, 0};
static const uint16_t pat_countdown[] = {30, 0};
static const uint16_t pat_reset[] = {80, 80, 80, 80, 80, 0};

static const buzz_pattern_t patterns[UI_BUZZ_COUNT] = {
    [UI_BUZZ_BOOT] = {pat_boot, 0},
    [UI_BUZZ_BAND] = {pat_band, 1},
    [UI_BUZZ_MODE] = {pat_mode, 1},
    [UI_BUZZ_CHANNEL] = {pat_channel, 0},
    [UI_BUZZ_TOP] = {pat_top, 0},
    [UI_BUZZ_BOTTOM] = {pat_bottom, 0},
    [UI_BUZZ_COUNTDOWN] = {pat_countdown, 0},
    [UI_BUZZ_RESET] = {pat_reset, 2},
};

static QueueHandle_t buzz_queue;
static bool buzz_enabled = true;

void buzzer_enable(bool enabled) { buzz_enabled = enabled; }

static void buzzer_task(void *arg) {
    (void)arg;
    uint8_t sound;
    for (;;) {
        if (xQueueReceive(buzz_queue, &sound, portMAX_DELAY) != pdTRUE) continue;
        uint8_t priority = patterns[sound].priority;
        const uint16_t *step = patterns[sound].pattern;
        gpio_set_level(BUZZER_PIN, 1);
        while (*step) {
            vTaskDelay(pdMS_TO_TICKS(*step));
            gpio_set_level(BUZZER_PIN, 0);
            step++;
            if (!*step) break;
            vTaskDelay(pdMS_TO_TICKS(*step));
            step++;
            gpio_set_level(BUZZER_PIN, 1);
            uint8_t next;
            if (xQueuePeek(buzz_queue, &next, 0) == pdTRUE && patterns[next].priority >= priority) break;
        }
        gpio_set_level(BUZZER_PIN, 0);
    }
}

void buzzer_init(void) {
    const gpio_config_t config = {
        .pin_bit_mask = UI_PIN_MASK(BUZZER_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&config);
    gpio_set_level(BUZZER_PIN, 0);
    buzz_queue = xQueueCreate(8, sizeof(uint8_t));
    if (!buzz_queue) return;
    xTaskCreate(buzzer_task, "ui_buzz", 2048, NULL, 4, NULL);
}

void buzzer_play(ui_buzz_t sound) {
    if (!buzz_enabled || !buzz_queue || sound >= UI_BUZZ_COUNT) return;
    uint8_t v = (uint8_t)sound;
    xQueueSend(buzz_queue, &v, 0);
}
