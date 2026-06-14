#pragma once
// Minimal Arduino driver for the CST9217 capacitive touch (I2C). Device-only.
// Protocol ported from Waveshare's esp_lcd_touch_cst9217 component.
#include <stdint.h>

bool touch_begin();                       // I2C + hardware reset; logs comms status
bool touch_read(uint16_t *x, uint16_t *y); // true if currently pressed (x,y in screen px)
uint8_t touch_points();                   // latest reported touch count (0 when released)
