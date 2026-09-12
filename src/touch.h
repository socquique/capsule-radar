#pragma once
// Capacitive touch, board-neutral interface. Device-only.
// Exactly one driver compiles per build, chosen by the board header:
//   TOUCH_DRIVER_CST9217 -> touch_cst9217.cpp  (1.75)
//   TOUCH_DRIVER_FT3168  -> touch_ft3168.cpp   (1.43)
#include <stdint.h>

bool touch_begin();                        // I2C + reset; logs comms status
bool touch_read(uint16_t *x, uint16_t *y); // true if currently pressed (x,y in screen px)
