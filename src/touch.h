#pragma once
// Capacitive touch, board-neutral interface. Device-only.
// Exactly one driver compiles per build, chosen by the board header:
//   TOUCH_DRIVER_CST9217 -> touch_cst9217.cpp  (1.75)
//   TOUCH_DRIVER_FT3168  -> touch_ft3168.cpp   (1.43)
#include <stdint.h>

// TOUCH_NODATA: the read failed (I2C error, or a frame the controller marked invalid), so
// nothing was learned about the finger. It is not a lift: reported as one, a single bad
// read in the middle of a swipe ended the swipe.
enum TouchRead { TOUCH_UP, TOUCH_DOWN, TOUCH_NODATA };

bool touch_begin();                             // I2C + reset; logs comms status
TouchRead touch_read(uint16_t *x, uint16_t *y); // TOUCH_DOWN: x,y set (screen px)
