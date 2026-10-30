#pragma once
// ST77916 QSPI LCD of the Waveshare ESP32-S3-Touch-LCD-1.85C V2 (BOARD_LCD185C_V2 only).
#include <Arduino_GFX_Library.h>

// Reset the panel through the TCA9554, read its ID, start it with the matching Waveshare
// init table and attach the backlight (dark until st77916_set_brightness()).
// nullptr if the panel did not start or its ID is not one of the two known revisions.
Arduino_GFX *st77916_begin();

void st77916_set_brightness(uint8_t v);   // 0..255, LEDC duty on PIN_LCD_BL
