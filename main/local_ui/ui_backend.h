#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Implemented by the C5 receiver backend. All calls are serialized against
 * host burst commands by the receiver's RF lock. */

/* Retune to mhz, capture n IQ samples at rate_code and return n log-power
 * codes in natural DFT order (index 0 is DC). Returns false if the radio is
 * busy or the capture failed. */
bool sdr_ui_spectrum(unsigned mhz, unsigned n, unsigned rate_code, uint8_t *codes, unsigned *elapsed_us);

/* Last frequency the receiver tuned to. */
unsigned sdr_ui_frequency(void);

/* True while a USB/UART host owns the radio or was seen recently. */
bool sdr_ui_host_active(void);
