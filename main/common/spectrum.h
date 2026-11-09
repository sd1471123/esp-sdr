/* Shared on-device spectrum protocol. Acquisition callbacks return normalized
 * IQ10 words (I in bits 0..9, Q in 10..19) in stable CPU-owned memory. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef bool (*spectrum_acquire_fn)(unsigned n, unsigned rate, const uint32_t **words, unsigned *elapsed_us);
bool spectrum_command(const char *line, unsigned frequency_mhz, spectrum_acquire_fn acquire);

bool spectrum_fft_init(void);

/* Analyse one captured IQ10 frame and write n 8-bit log-power codes in natural
 * DFT order (index 0 is DC). The caller must serialize access to the shared
 * FFT workspace, e.g. with the receiver's RF lock. */
bool spectrum_snapshot(const uint32_t *words, unsigned n, uint8_t *codes);

/* Snapshot and continuous backends run exclusively. Reuse their large buffers
 * on SRAM-constrained targets; acquiring this workspace invalidates the cached
 * snapshot window so a later transport switch rebuilds it. */
typedef struct {
    int16_t *fft, *window;
    float *power;
    uint8_t *frame;
} spectrum_workspace_t;
spectrum_workspace_t spectrum_workspace(void);
