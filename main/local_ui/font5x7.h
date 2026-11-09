#pragma once
#include <stdint.h>

#define FONT5X7_W 5
#define FONT5X7_H 7

/* ASCII 0x20..0x7e. Each row is a bit mask, bit 0 = leftmost column. */
extern const uint8_t font5x7[95][FONT5X7_H];
