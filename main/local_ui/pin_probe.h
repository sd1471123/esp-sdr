/* Buzzer-only header probe. Ground a pin, hear its GPIO number. */
#pragma once

#if CONFIG_ESP_SDR_PIN_PROBE
void pin_probe_run(void) __attribute__((noreturn));
#endif
