#pragma once
#include <stdint.h>

#define FONT7X12_W 7
#define FONT7X12_H 12

/* ASCII 0x20..0x7e. Each row is a bit mask, bit 0 = leftmost column. */
extern const uint8_t font7x12[95][FONT7X12_H];
